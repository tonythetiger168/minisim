// devices.hpp - the bundled peripheral models of a virtual platform.
//
//   ram       plain memory, read/write, optionally loaded from a file
//   rom       plain memory, read-only, loaded from a file
//   uart16550 a 16550 UART: transmit to the console, receive from a string
//   clint     the RISC-V core-local interruptor (msip, mtimecmp, mtime)
//   plic      the RISC-V platform-level interrupt controller
//   finisher  the SiFive test finisher: a write ends the run with an exit code
//
// Register layouts follow the parts Linux, Zephyr, FreeRTOS and the RISC-V
// test suites already have drivers for, so the software runs unchanged.
#pragma once

#include <sys/mman.h>

#include <deque>
#include <fstream>
#include <sstream>

#include "bus.hpp"

namespace vp {

inline uint64_t rdLE(const uint8_t* p, unsigned n) {
    uint64_t v = 0;
    memcpy(&v, p, n);
    return v;
}
inline void wrLE(uint8_t* p, unsigned n, uint64_t v) { memcpy(p, &v, n); }

class Ram : public Device {
public:
    Ram(uint64_t sz, bool rw) : mem_(sz), rw_(rw) { size = sz; type = rw ? "ram" : "rom"; }
    Acc read(uint64_t off, unsigned n, uint64_t& v, const Who&) override {
        v = rdLE(&mem_[off], n);
        return Acc::Ok;
    }
    Acc write(uint64_t off, unsigned n, uint64_t v, const Who&) override {
        if (!rw_) return Acc::Fault;
        wrLE(&mem_[off], n, v);
        return Acc::Ok;
    }
    uint8_t* memory() override { return mem_.data(); }
    bool writable() const override { return rw_; }
    bool ordered() const override { return false; }
    void loadFile(const std::string& path, uint64_t off) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("'" + name + "': cannot read " + path);
        std::vector<char> b((std::istreambuf_iterator<char>(in)), {});
        if (off + b.size() > mem_.size())
            throw std::runtime_error("'" + name + "': " + path + " (" + std::to_string(b.size()) +
                                     " bytes) does not fit");
        memcpy(&mem_[off], b.data(), b.size());
    }
    // Only the 4 KiB pages that hold something are saved.
    void save(Writer& w) const override {
        const size_t pg = 4096;
        uint64_t n = 0;
        for (size_t o = 0; o < mem_.size(); o += pg) n += !zero(o, std::min(pg, mem_.size() - o));
        w.u64(n);
        for (size_t o = 0; o < mem_.size(); o += pg) {
            size_t len = std::min(pg, mem_.size() - o);
            if (zero(o, len)) continue;
            w.u64(o);
            w.bytes(&mem_[o], len);
        }
    }
    void load(Reader& r) override {
        mem_.clear();
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            uint64_t o = r.u64();
            if (o >= mem_.size()) throw std::runtime_error("the checkpoint does not fit '" + name + "'");
            r.read(&mem_[o], std::min<size_t>(4096, mem_.size() - o));
        }
    }

private:
    bool zero(size_t o, size_t len) const {
        for (size_t i = 0; i < len; i++) if (mem_[o + i]) return false;
        return true;
    }
    // Zero pages from the kernel, taken on first touch: a 2 GiB RAM costs
    // nothing until the software uses it.
    struct Pages {
        uint8_t* p = nullptr;
        size_t n = 0;
        explicit Pages(size_t sz) : n(sz) {
            void* m = sz ? mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)
                         : nullptr;
            if (m == MAP_FAILED) throw std::runtime_error("cannot map " + std::to_string(sz) + " bytes of memory");
            p = (uint8_t*)m;
        }
        ~Pages() { if (p) munmap(p, n); }
        Pages(const Pages&) = delete;
        Pages& operator=(const Pages&) = delete;
        uint8_t& operator[](size_t i) { return p[i]; }
        const uint8_t& operator[](size_t i) const { return p[i]; }
        uint8_t* data() { return p; }
        size_t size() const { return n; }
        void clear() {
            if (n && mmap(p, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED)
                memset(p, 0, n);
        }
    };
    Pages mem_;
    bool rw_;
};

// 16550: registers at offsets (index << shift). Transmission is instant: the
// point of a virtual platform is the software, and a driver that waits for
// THRE sees it set at once. Received characters come from `input`, one each
// time the receive buffer is read.
class Uart16550 : public Device {
public:
    unsigned shift = 0, irq = 0;
    std::deque<uint8_t> rx;

    Uart16550() { type = "uart16550"; }
    Acc read(uint64_t off, unsigned, uint64_t& v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        unsigned r = (unsigned)(off >> shift);
        v = 0;
        switch (r) {
            case 0:
                if (lcr_ & 0x80) v = dll_;
                else if (!rx.empty()) { v = rx.front(); rx.pop_front(); update(); }
                break;
            case 1: v = (lcr_ & 0x80) ? dlm_ : ier_; break;
            case 2: {
                unsigned id = 0x01;  // nothing pending
                if ((ier_ & 1) && !rx.empty()) id = 0x04;
                else if ((ier_ & 2) && thri_) { id = 0x02; thri_ = false; update(); }
                v = 0xc0 | id;
                break;
            }
            case 3: v = lcr_; break;
            case 4: v = mcr_; break;
            case 5: v = 0x60 | (rx.empty() ? 0 : 1); break;  // THRE | TEMT | DR
            case 6: v = 0xb0; break;                           // DCD, DSR, CTS
            case 7: v = scr_; break;
        }
        return Acc::Ok;
    }
    Acc write(uint64_t off, unsigned, uint64_t v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        unsigned r = (unsigned)(off >> shift);
        uint8_t b = (uint8_t)v;
        switch (r) {
            case 0:
                if (lcr_ & 0x80) dll_ = b;
                else { p->out(std::string(1, (char)b)); thri_ = true; tx_++; update(); }
                break;
            case 1:
                if (lcr_ & 0x80) dlm_ = b;
                else { bool was = ier_ & 2; ier_ = b & 0x0f; if (!was && (ier_ & 2)) thri_ = true; update(); }
                break;
            case 2: break;  // FCR: FIFOs are always on
            case 3: lcr_ = b; break;
            case 4: mcr_ = b; break;
            case 7: scr_ = b; break;
        }
        return Acc::Ok;
    }
    void start() override { update(); }
    void save(Writer& w) const override {
        w.u32(ier_ | lcr_ << 8 | mcr_ << 16 | scr_ << 24);
        w.u32(dll_ | dlm_ << 8 | (thri_ ? 1u << 16 : 0));
        w.u64(rx.size());
        for (uint8_t c : rx) w.bytes(&c, 1);
    }
    void load(Reader& r) override {
        uint32_t a = r.u32(), b = r.u32();
        ier_ = a & 0xff; lcr_ = (a >> 8) & 0xff; mcr_ = (a >> 16) & 0xff; scr_ = a >> 24;
        dll_ = b & 0xff; dlm_ = (b >> 8) & 0xff; thri_ = (b >> 16) & 1;
        rx.clear();
        for (uint64_t n = r.u64(); n; n--) { uint8_t c; r.read(&c, 1); rx.push_back(c); }
        update();
    }
    uint64_t transmitted() const { return tx_; }

private:
    void update() {
        bool lvl = ((ier_ & 1) && !rx.empty()) || ((ier_ & 2) && thri_);
        if (irq) p->setIrq(irq, lvl);
    }
    uint8_t ier_ = 0, lcr_ = 0, mcr_ = 0, scr_ = 0, dll_ = 0, dlm_ = 0;
    bool thri_ = false;
    uint64_t tx_ = 0;
};

// CLINT (SiFive layout): msip[h] at 4*h, mtimecmp[h] at 0x4000 + 8*h, mtime
// at 0xbff8. mtime counts ticks of `tickPs`, derived from the asking hart's
// cycle count, so a hart reads the time it has reached.
class Clint : public Device {
public:
    std::vector<uint32_t> msip;
    std::vector<uint64_t> mtimecmp;
    int64_t offset = 0;  // what software wrote into mtime, against the cycle count

    Clint() { type = "clint"; size = 0x10000; }
    void start() override {
        msip.assign(p->hartCount(), 0);
        mtimecmp.assign(p->hartCount(), UINT64_MAX);
    }
    uint64_t mtimeAtCycle(uint64_t cycle) const {
        return (uint64_t)((int64_t)(cycle * p->cyclePs() / p->tickPs()) + offset);
    }
    // The first cycle at which mtime >= cmp.
    uint64_t cycleOf(uint64_t cmp) const {
        if (cmp == UINT64_MAX) return UINT64_MAX;
        int64_t ticks = (int64_t)cmp - offset;
        if (ticks <= 0) return 0;
        unsigned __int128 ps = (unsigned __int128)(uint64_t)ticks * p->tickPs();
        unsigned __int128 c = (ps + p->cyclePs() - 1) / p->cyclePs();
        return c > UINT64_MAX ? UINT64_MAX : (uint64_t)c;
    }
    Acc read(uint64_t off, unsigned n, uint64_t& v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        v = 0;
        if (off < 0x4000) {
            unsigned h = (unsigned)(off / 4);
            if (h < msip.size()) v = msip[h];
        } else if (off >= 0x4000 && off < 0xbff8) {
            unsigned h = (unsigned)((off - 0x4000) / 8);
            if (h < mtimecmp.size()) v = mtimecmp[h] >> (8 * (off & 7));
        } else if (off >= 0xbff8) {
            v = mtimeAtCycle(w.cycle) >> (8 * (off - 0xbff8));
        }
        if (n < 8) v &= (1ull << (8 * n)) - 1;
        return Acc::Ok;
    }
    Acc write(uint64_t off, unsigned n, uint64_t v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        if (off < 0x4000) {
            unsigned h = (unsigned)(off / 4);
            if (h < msip.size()) msip[h] = v & 1;
        } else if (off >= 0x4000 && off < 0xbff8) {
            unsigned h = (unsigned)((off - 0x4000) / 8);
            if (h < mtimecmp.size()) mtimecmp[h] = merge(mtimecmp[h], off & 7, n, v);
        } else if (off >= 0xbff8) {
            uint64_t now = mtimeAtCycle(w.cycle);
            uint64_t want = merge(now, off - 0xbff8, n, v);
            offset += (int64_t)(want - now);
        }
        p->irqChanged();
        return Acc::Ok;
    }
    void save(Writer& w) const override {
        w.u64((uint64_t)offset);
        w.u64(msip.size());
        for (size_t i = 0; i < msip.size(); i++) { w.u32(msip[i]); w.u64(mtimecmp[i]); }
    }
    void load(Reader& r) override {
        offset = (int64_t)r.u64();
        uint64_t n = r.u64();
        if (n != msip.size()) throw std::runtime_error("the checkpoint has " + std::to_string(n) + " harts");
        for (size_t i = 0; i < n; i++) { msip[i] = r.u32(); mtimecmp[i] = r.u64(); }
    }

private:
    static uint64_t merge(uint64_t old, uint64_t byteOff, unsigned n, uint64_t v) {
        uint64_t mask = (n >= 8 ? ~0ull : ((1ull << (8 * n)) - 1)) << (8 * byteOff);
        return (old & ~mask) | ((v << (8 * byteOff)) & mask);
    }
};

// PLIC (SiFive layout), one M-mode context per hart. Sources are level
// sensitive: a source is pending while its line is high and it is not being
// serviced; a claim takes the highest-priority pending enabled source above
// the context's threshold (the lowest id on a tie); a complete ends service.
class Plic : public Device {
public:
    unsigned nsrc = 32;
    std::vector<uint8_t> level;  // driven by the platform

    Plic() { type = "plic"; size = 0x4000000; }
    void start() override {
        level.assign(nsrc + 1, 0);
        prio_.assign(nsrc + 1, 0);
        busy_.assign(nsrc + 1, 0);
        unsigned words = (nsrc + 32) / 32;
        en_.assign(p->hartCount(), std::vector<uint32_t>(words, 0));
        thr_.assign(p->hartCount(), 0);
    }
    bool pending(unsigned s) const { return s && s <= nsrc && level[s] && !busy_[s]; }
    // Does context `c` see an interrupt?
    bool eip(unsigned c) const {
        for (unsigned s = 1; s <= nsrc; s++)
            if (pending(s) && enabled(c, s) && prio_[s] > thr_[c]) return true;
        return false;
    }
    Acc read(uint64_t off, unsigned, uint64_t& v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        v = 0;
        if (off < 0x1000) {
            unsigned s = (unsigned)(off / 4);
            if (s <= nsrc) v = prio_[s];
        } else if (off < 0x2000) {
            unsigned word = (unsigned)((off - 0x1000) / 4);
            for (unsigned b = 0; b < 32; b++)
                if (pending(word * 32 + b)) v |= 1ull << b;
        } else if (off < 0x200000) {
            unsigned c = (unsigned)((off - 0x2000) / 0x80), word = (unsigned)((off - 0x2000) % 0x80 / 4);
            if (c < en_.size() && word < en_[c].size()) v = en_[c][word];
        } else {
            unsigned c = (unsigned)((off - 0x200000) / 0x1000), r = (unsigned)((off - 0x200000) % 0x1000);
            if (c < thr_.size()) {
                if (r == 0) v = thr_[c];
                else if (r == 4) v = claim(c);
            }
        }
        return Acc::Ok;
    }
    Acc write(uint64_t off, unsigned, uint64_t v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        if (off < 0x1000) {
            unsigned s = (unsigned)(off / 4);
            if (s && s <= nsrc) prio_[s] = (uint32_t)v & 7;
        } else if (off < 0x2000) {
            // pending bits are read-only
        } else if (off < 0x200000) {
            unsigned c = (unsigned)((off - 0x2000) / 0x80), word = (unsigned)((off - 0x2000) % 0x80 / 4);
            if (c < en_.size() && word < en_[c].size()) en_[c][word] = (uint32_t)v & (word ? ~0u : ~1u);
        } else {
            unsigned c = (unsigned)((off - 0x200000) / 0x1000), r = (unsigned)((off - 0x200000) % 0x1000);
            if (c < thr_.size()) {
                if (r == 0) thr_[c] = (uint32_t)v & 7;
                else if (r == 4 && v && v <= nsrc && enabled(c, (unsigned)v)) busy_[v] = 0;
            }
        }
        p->irqChanged();
        return Acc::Ok;
    }
    void save(Writer& w) const override {
        w.u64(nsrc);
        for (unsigned s = 0; s <= nsrc; s++) w.u32(prio_[s] | busy_[s] << 8);
        for (size_t c = 0; c < en_.size(); c++) {
            w.u32(thr_[c]);
            for (uint32_t e : en_[c]) w.u32(e);
        }
    }
    void load(Reader& r) override {
        if (r.u64() != nsrc) throw std::runtime_error("the checkpoint's PLIC has another source count");
        for (unsigned s = 0; s <= nsrc; s++) { uint32_t x = r.u32(); prio_[s] = x & 0xff; busy_[s] = (x >> 8) & 1; }
        for (size_t c = 0; c < en_.size(); c++) {
            thr_[c] = r.u32();
            for (auto& e : en_[c]) e = r.u32();
        }
    }

private:
    bool enabled(unsigned c, unsigned s) const { return (en_[c][s / 32] >> (s % 32)) & 1; }
    uint32_t claim(unsigned c) {
        unsigned best = 0;
        for (unsigned s = 1; s <= nsrc; s++)
            if (pending(s) && enabled(c, s) && prio_[s] > thr_[c] && (!best || prio_[s] > prio_[best])) best = s;
        if (best) { busy_[best] = 1; p->irqChanged(); }
        return best;
    }
    std::vector<uint32_t> prio_;
    std::vector<uint8_t> busy_;
    std::vector<std::vector<uint32_t>> en_;
    std::vector<uint32_t> thr_;
};

// SiFive test finisher: 0x5555 passes (exit 0), 0x3333 | code << 16 fails
// with that code, 0x7777 (reset) ends the run with exit 0 as well.
class Finisher : public Device {
public:
    Finisher() { type = "finisher"; size = 0x1000; }
    Acc read(uint64_t, unsigned, uint64_t& v, const Who&) override { v = 0; return Acc::Ok; }
    Acc write(uint64_t off, unsigned, uint64_t v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        if (off != 0) return Acc::Ok;
        unsigned st = v & 0xffff;
        if (st == 0x5555 || st == 0x7777) p->requestExit(0);
        else if (st == 0x3333) p->requestExit((int)((v >> 16) & 0xffff) ? (int)((v >> 16) & 0xffff) : 1);
        return Acc::Ok;
    }
};

}  // namespace vp
