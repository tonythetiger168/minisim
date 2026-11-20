// machine.hpp - a virtual platform: harts, the memory map, time, interrupts,
// and the instruction loop.
//
// Time. Every hart retires one instruction per cycle of `cyclePs`. The
// platform advances in quanta (temporal decoupling, as SystemC TLM-2.0
// loosely-timed models do): each hart runs up to the end of the quantum on
// its own, then devices see time move and interrupts are re-evaluated. A
// quantum also ends at the next timer compare, so a timer interrupt arrives
// on its cycle whatever the quantum.
//
// Several harts. A quantum is run in rounds. In the parallel phase each hart
// runs on a thread with its own write buffer (Overlay), until its budget runs
// out or it reaches an access with side effects (a peripheral register, an
// atomic, LR/SC, HTIF). Then, in the ordered phase, the buffers are written
// back in hart order and each stopped hart performs its one access, in hart
// order. Nothing depends on the thread count or the OS scheduler, so the run
// is the same bit for bit on 1 thread or 64.
#pragma once

#include <chrono>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "bus.hpp"
#include "devices.hpp"
#include "elf.hpp"
#include "fpu.hpp"
#include "jit.hpp"
#include "parallel.hpp"
#include "rv32.hpp"

namespace vp {

// The other end of a debugger connection (gdb.hpp), seen from the machine.
struct Debugger {
    virtual ~Debugger() = default;
    // A hart stopped (breakpoint, watchpoint, step, or the start of the run).
    // Returns when the debugger lets the machine run again.
    virtual void stopped(unsigned hart) = 0;
    // Between quanta: has the debugger asked to interrupt (Ctrl-C)?
    virtual bool interrupted() = 0;
    // The run is over.
    virtual void exited(int code) = 0;
};

// The bridge to RTL in msim (cosim.hpp), seen from the machine.
struct Cosim {
    virtual ~Cosim() = default;
    // A device returned Pending in the ordered phase: complete its access.
    // Returns the time the access finished, which the hart waits for.
    virtual uint64_t service(uint64_t nowPs) = 0;
};

class Machine : public Platform {
public:
    Bus bus;
    std::vector<std::unique_ptr<Hart>> harts;
    uint64_t cycle_ps = 10000;  // 100 MHz
    uint64_t tick_ps = 100000;  // mtime at 10 MHz
    uint64_t quantum = 1000;    // cycles
    uint64_t maxCycles = 0;     // 0: no limit
    unsigned threads = 1;
    Clint* clint = nullptr;
    Plic* plic = nullptr;
    Cosim* cosim = nullptr;
    Debugger* dbg = nullptr;
    Elf elf;
    bool haveElf = false;
    uint64_t tohost = 0;  // HTIF: 0 when the program has no tohost symbol
    bool timeout = false;

    // -trace and -profile.
    FILE* trace = nullptr;
    bool profile = false;
    // The dynamic translator (x86-64 hosts): on unless -nojit.
    bool jit = true;
    // Several harts take turns on one thread, a quantum each, storing to
    // memory directly (no write buffers, no ordered phase): another
    // interleaving than the parallel rounds', also the same on every run,
    // and each hart runs on the fast loop and the translator.
    bool serial = false;

    // Debugger state.
    std::set<uint64_t> breakpoints;
    struct Watch { uint64_t addr, len; int kind; };
    std::vector<Watch> watches;

    // --- Platform
    void setIrq(unsigned src, bool level) override {
        if (plic && src && src < plic->level.size() && plic->level[src] != (uint8_t)level) {
            plic->level[src] = level;
            irqDirty_ = true;
        }
    }
    unsigned hartCount() const override { return (unsigned)harts.size(); }
    uint64_t tickPs() const override { return tick_ps; }
    uint64_t cyclePs() const override { return cycle_ps; }
    void requestExit(int code) override {
        if (!exit_) { exit_ = true; exitCode_ = code; }
    }
    void irqChanged() override { irqDirty_ = true; }
    // DMA: plain memory directly (the harts see code changed this way after
    // FENCE.I, as for any other store), other devices as a serial access.
    bool masterRead(uint64_t a, uint8_t& v, const Device* from) override {
        Device* d = bus.find(a);
        if (!d || d == from) return false;
        if (d->memory()) { v = d->memory()[a - d->base]; return true; }
        uint64_t t = 0;
        Who w{0, harts[0]->cycle, true};
        if (d->read(a - d->base, 1, t, w) != Acc::Ok) return false;
        d->nread++;
        v = (uint8_t)t;
        return true;
    }
    bool masterWrite(uint64_t a, uint8_t v, const Device* from) override {
        Device* d = bus.find(a);
        if (!d || d == from) return false;
        if (d->memory()) {
            if (!d->writable()) return false;
            d->memory()[a - d->base] = v;
            if (tohost && a >= tohost && a < tohost + 8) htif(*harts[0]);
            return true;
        }
        Who w{0, harts[0]->cycle, true};
        if (d->write(a - d->base, 1, v, w) != Acc::Ok) return false;
        d->nwrite++;
        return true;
    }
    uint8_t* memSpan(uint64_t a, uint64_t n, bool write) override {
        Device* d = bus.find(a);
        if (!d || !d->memory() || (write && !d->writable()) || a + n > d->base + d->size || a + n < a) return nullptr;
        if (write && tohost && a < tohost + 8 && a + n > tohost) return nullptr;
        return d->memory() + (a - d->base);
    }
    void out(const std::string& s) override {
        fwrite(s.data(), 1, s.size(), stdout);
        if (s.find('\n') != std::string::npos) fflush(stdout);
        consoleBytes_ += s.size();
    }

    bool exited() const { return exit_; }
    int exitCode() const { return exitCode_; }
    uint64_t now() const { return gcycle_; }
    // The furthest any hart got: the run's length when it ended mid-quantum.
    uint64_t cycles() const {
        uint64_t c = gcycle_;
        for (auto& h : harts) c = std::max(c, h->cycle);
        return c;
    }
    uint64_t nowPs() const { return gcycle_ * cycle_ps; }

    void addHarts(unsigned n, uint64_t resetPc) {
        for (unsigned i = 0; i < n; i++) {
            auto h = std::make_unique<Hart>();
            h->id = i;
            h->pc = resetPc;
            harts.push_back(std::move(h));
        }
    }

    // Put bytes into plain memory (loading a program). Throws if no RAM or
    // ROM holds them.
    void poke(uint64_t a, const uint8_t* p, size_t n) {
        while (n) {
            Device* d = bus.find(a);
            if (!d || !d->memory())
                throw std::runtime_error("no memory at " + Bus::hex(a) + " to load the program into");
            size_t k = std::min<uint64_t>(n, d->base + d->size - a);
            memcpy(d->memory() + (a - d->base), p, k);
            a += k, p += k, n -= k;
        }
        for (auto& h : harts) h->flushDecoded();
    }
    void loadElf(const std::string& path) {
        elf = readElf(path);
        haveElf = true;
        for (auto& s : elf.segs) poke(s.addr, s.data.data(), s.data.size());
        for (auto& h : harts) { h->pc = elf.entry; h->rv64 = elf.is64; }
        auto it = elf.syms.find("tohost");
        if (it != elf.syms.end()) tohost = it->second;
    }

    // Debugger memory access: reads and writes through the map with no side
    // effects on plain memory and none counted.
    bool debugRead(uint64_t a, uint8_t& v) {
        Device* d = bus.find(a);
        if (!d) return false;
        if (d->memory()) { v = d->memory()[a - d->base]; return true; }
        uint64_t t;
        Who w{0, harts[0]->cycle, true};
        if (d->read(a - d->base, 1, t, w) != Acc::Ok) return false;
        v = (uint8_t)t;
        return true;
    }
    bool debugWrite(uint64_t a, uint8_t v) {
        Device* d = bus.find(a);
        if (!d) return false;
        if (d->memory()) {
            d->memory()[a - d->base] = v;
            for (auto& h : harts) h->flushDecoded();
            return true;
        }
        Who w{0, harts[0]->cycle, true};
        return d->write(a - d->base, 1, v, w) == Acc::Ok;
    }

    void start() {
        for (auto& d : bus.devices()) d->p = this;
        for (auto& d : bus.devices()) d->start();
        if (harts.size() > 1)
            for (auto& d : bus.devices())
                if (d->memory() && ((d->base | d->size) & 4095))
                    throw std::runtime_error("'" + d->name + "': with more than one hart, memory must start and end on a 4 KiB boundary");
        updateIrqs();
        if (harts.size() > 1 && threads > 1 && !serial) {
            pool_ = std::make_unique<par::Pool>(std::min<unsigned>(threads, (unsigned)harts.size()));
            pool_->spin(20000);
        }
        wall0_ = std::chrono::steady_clock::now();
    }

    // Run until a device or the program ends the run, or maxCycles.
    void run() {
        if (dbg) { dbg->stopped(0); }
        while (!exit_) {
            uint64_t end = gcycle_ + quantum;
            uint64_t ev = nextEventCycle();
            // One hart and no device that keeps time of its own: nothing
            // happens at quantum boundaries until the hart touches a device,
            // so run up to 64 quanta at once, cut back to the boundary after
            // the first device access (devRead, devWrite).
            merge_ = harts.size() == 1 && !dbg && !cosim && !anyLive();
            if (merge_) end = gcycle_ + quantum * 64;
            cut_ = UINT64_MAX;
            if (ev > gcycle_ && ev < end) end = ev;
            bool allWfi = true;
            for (auto& h : harts) allWfi = allWfi && h->wfi;
            if (allWfi && !cosim && !dbg && !hasTimedDevice()) {
                // Nothing can happen before the next timer compare: skip to it.
                if (ev == UINT64_MAX) {
                    fprintf(stderr, "mvplat: every hart waits for an interrupt and none can come; stopping\n");
                    requestExit(3);
                    break;
                }
                if (ev > end) end = ev;
            }
            if (maxCycles && end > maxCycles) end = maxCycles;
            if (end <= gcycle_) {
                timeout = true;
                requestExit(4);
                break;
            }
            if (harts.size() == 1) runOne(*harts[0], end);
            else if (serial) runSerial(end);
            else runRound(end);
            if (exit_) break;
            if (cut_ < end) end = cut_;
            gcycle_ = end;
            for (auto& d : bus.devices()) d->advance(nowPs());
            // The lines only change through irqChanged/setIrq (dirty) or as
            // time reaches an event, which ends the quantum.
            if (irqDirty_ || (ev != UINT64_MAX && ev <= end)) updateIrqs();
            if (dbg && dbg->interrupted()) {
                for (auto& h : harts) h->stopSig = 2;
                dbg->stopped(0);
            }
        }
        for (auto& d : bus.devices()) d->finish();
        wall1_ = std::chrono::steady_clock::now();
        if (dbg) dbg->exited(exitCode_);
    }

    // ------------------------------------------------------------ checkpoints
    // The platform's own time, for a checkpoint writer: every hart's state
    // is in `harts` and every device's in Device::save/load.
    uint64_t platformCycle() const { return gcycle_; }
    void setPlatformCycle(uint64_t c) { gcycle_ = c; }

    double seconds() const { return std::chrono::duration<double>(wall1_ - wall0_).count(); }
    uint64_t consoleBytes() const { return consoleBytes_; }

    // mip from the CLINT and the PLIC, for every hart.
    void updateIrqs() {
        irqDirty_ = false;
        for (auto& hp : harts) {
            Hart& h = *hp;
            uint32_t mip = h.mipSw;
            if (clint) {
                if (clint->msip[h.id]) mip |= MIP_MSIP;
                if (clint->mtimeAtCycle(h.cycle) >= clint->mtimecmp[h.id]) mip |= MIP_MTIP;
            }
            if (plic && plic->eip(2 * h.id)) mip |= MIP_MEIP;
            if (plic && plic->eip(2 * h.id + 1)) mip |= MIP_SEIP;
            h.mip = mip;
            if (h.wfi && (h.mip & h.mie)) h.wfi = false;
        }
    }

    // ------------------------------------------------------------ CSRs
    uint64_t mtime(const Hart& h) const {
        return clint ? clint->mtimeAtCycle(h.cycle) : h.cycle * cycle_ps / tick_ps;
    }
    // The extensions misa reports: A C D F I M S U.
    // Zba, Zbb, Zbc, Zbs and Zicond (unprivileged ISA, chapters 28 and 12):
    // the OP, OP-IMM, OP-32 and OP-IMM-32 encodings the base ISA leaves
    // free. Values are XLEN wide; the caller sign-extends an RV32 result.
    // Returns false for an encoding that is none of them.
    static bool bitmanip(uint32_t insn, bool rv64, uint64_t a, uint64_t b, uint64_t& r) {
        const unsigned op = insn & 0x7f, f3 = (insn >> 12) & 7, f7 = insn >> 25, rs2 = (insn >> 20) & 31;
        const unsigned xl = rv64 ? 64 : 32;
        const uint64_t m = rv64 ? ~0ull : 0xffffffffull;
        a &= m; b &= m;
        auto rol = [&](uint64_t v, unsigned s, unsigned w) -> uint64_t {
            uint64_t mm = w == 64 ? ~0ull : (1ull << w) - 1;
            s &= w - 1;
            v &= mm;
            return s ? ((v << s) | (v >> (w - s))) & mm : v;
        };
        auto clz = [&](uint64_t v, unsigned w) -> uint64_t {
            v &= w == 64 ? ~0ull : (1ull << w) - 1;
            return v ? (uint64_t)(__builtin_clzll(v) - (64 - w)) : w;
        };
        auto ctz = [&](uint64_t v, unsigned w) -> uint64_t {
            v &= w == 64 ? ~0ull : (1ull << w) - 1;
            return v ? (uint64_t)__builtin_ctzll(v) : w;
        };
        auto clmul = [&](uint64_t x, uint64_t y, int kind) -> uint64_t {  // 0 low, 1 high, 2 reversed
            uint64_t out = 0;
            for (unsigned i = 0; i < xl; i++) {
                if (!((y >> i) & 1)) continue;
                if (kind == 0) out ^= x << i;
                else if (kind == 1) out ^= i ? x >> (xl - i) : 0;
                else out ^= x >> (xl - i - 1);
            }
            return out & m;
        };
        auto sext = [&](uint64_t v, unsigned w) -> uint64_t { return (uint64_t)((int64_t)(v << (64 - w)) >> (64 - w)) & m; };
        const unsigned sh = (insn >> 20) & (xl - 1);
        const unsigned f6 = insn >> 26;
        const unsigned f12 = insn >> 20;
        switch (op) {
            case 0x33:
                switch (f7) {
                    case 0x10:
                        if (f3 == 2) { r = ((a << 1) + b) & m; return true; }
                        if (f3 == 4) { r = ((a << 2) + b) & m; return true; }
                        if (f3 == 6) { r = ((a << 3) + b) & m; return true; }
                        return false;
                    case 0x20:
                        if (f3 == 7) { r = a & ~b & m; return true; }
                        if (f3 == 6) { r = (a | ~b) & m; return true; }
                        if (f3 == 4) { r = ~(a ^ b) & m; return true; }
                        return false;
                    case 0x05: {
                        int64_t sa = (int64_t)(rv64 ? a : (uint64_t)(int64_t)(int32_t)a);
                        int64_t sb = (int64_t)(rv64 ? b : (uint64_t)(int64_t)(int32_t)b);
                        switch (f3) {
                            case 1: r = clmul(a, b, 0); return true;
                            case 2: r = clmul(a, b, 2); return true;
                            case 3: r = clmul(a, b, 1); return true;
                            case 4: r = (uint64_t)(sa < sb ? sa : sb) & m; return true;
                            case 5: r = a < b ? a : b; return true;
                            case 6: r = (uint64_t)(sa > sb ? sa : sb) & m; return true;
                            case 7: r = a > b ? a : b; return true;
                        }
                        return false;
                    }
                    case 0x30:
                        if (f3 == 1) { r = rol(a, (unsigned)b, xl); return true; }
                        if (f3 == 5) { r = rol(a, (xl - (unsigned)(b & (xl - 1))) & (xl - 1), xl); return true; }
                        return false;
                    case 0x24:
                        if (f3 == 1) { r = a & ~(1ull << (b & (xl - 1))); return true; }
                        if (f3 == 5) { r = (a >> (b & (xl - 1))) & 1; return true; }
                        return false;
                    case 0x14:
                        if (f3 == 1) { r = a | (1ull << (b & (xl - 1))); return true; }
                        if (f3 == 2 || f3 == 4) {                          // xperm4, xperm8 (Zbkx)
                            const unsigned e = f3 == 2 ? 4 : 8;
                            const uint64_t em = (1ull << e) - 1;
                            uint64_t o = 0;
                            for (unsigned i = 0; i < xl; i += e) {
                                uint64_t idx = (b >> i) & em;
                                if (idx * e < xl) o |= ((a >> (idx * e)) & em) << i;
                            }
                            r = o;
                            return true;
                        }
                        return false;
                    case 0x34: if (f3 == 1) { r = a ^ (1ull << (b & (xl - 1))); return true; } return false;
                    case 0x04: {  // pack, packh (Zbkb); RV32 zext.h is pack with rs2 = x0
                        const unsigned hx = xl / 2;
                        const uint64_t hm = (1ull << hx) - 1;
                        if (f3 == 4) { r = ((a & hm) | ((b & hm) << hx)) & m; return true; }
                        if (f3 == 7) { r = (a & 0xff) | ((b & 0xff) << 8); return true; }
                        return false;
                    }
                    case 0x07:
                        if (f3 == 5) { r = b == 0 ? 0 : a; return true; }   // czero.eqz
                        if (f3 == 7) { r = b != 0 ? 0 : a; return true; }   // czero.nez
                        return false;
                }
                return false;
            case 0x13:
                if (f3 == 1) {
                    if (f12 == 0x600) { r = clz(a, xl); return true; }
                    if (f12 == 0x601) { r = ctz(a, xl); return true; }
                    if (f12 == 0x602) { r = (uint64_t)__builtin_popcountll(a); return true; }
                    if (f12 == 0x604) { r = sext(a, 8); return true; }
                    if (f12 == 0x605) { r = sext(a, 16); return true; }
                    if (!rv64 && f12 == 0x08f) {  // zip (Zbkb, RV32)
                        uint64_t o = 0;
                        for (unsigned i = 0; i < 16; i++) o |= ((a >> i) & 1) << (2 * i) | ((a >> (i + 16)) & 1) << (2 * i + 1);
                        r = o;
                        return true;
                    }
                    unsigned hi = rv64 ? f6 << 1 : f7;  // funct7 with RV64's shamt[5] cleared
                    if (hi == 0x14) { r = a | (1ull << sh); return true; }   // bseti
                    if (hi == 0x24) { r = a & ~(1ull << sh); return true; }  // bclri
                    if (hi == 0x34) { r = a ^ (1ull << sh); return true; }   // binvi
                    return false;
                }
                if (f3 == 5) {
                    if (f12 == 0x287) {  // orc.b
                        uint64_t o = 0;
                        for (unsigned i = 0; i < xl; i += 8) if ((a >> i) & 0xff) o |= 0xffull << i;
                        r = o;
                        return true;
                    }
                    if (f12 == 0x687) {  // brev8 (Zbkb)
                        uint64_t o = 0;
                        for (unsigned i = 0; i < xl; i++) if ((a >> i) & 1) o |= 1ull << ((i & ~7u) | (7 - (i & 7)));
                        r = o;
                        return true;
                    }
                    if (!rv64 && f12 == 0x08f) {  // unzip (Zbkb, RV32)
                        uint64_t o = 0;
                        for (unsigned i = 0; i < 16; i++) o |= ((a >> (2 * i)) & 1) << i | ((a >> (2 * i + 1)) & 1) << (i + 16);
                        r = o;
                        return true;
                    }
                    if ((rv64 && f12 == 0x6b8) || (!rv64 && f12 == 0x698)) {  // rev8
                        r = rv64 ? __builtin_bswap64(a) : __builtin_bswap32((uint32_t)a);
                        return true;
                    }
                    unsigned hi = rv64 ? f6 << 1 : f7;
                    if (hi == 0x30) { r = rol(a, (xl - sh) & (xl - 1), xl); return true; }  // rori
                    if (hi == 0x24) { r = (a >> sh) & 1; return true; }                     // bexti
                    return false;
                }
                return false;
            case 0x1b: {  // RV64 only
                uint32_t w = (uint32_t)a;
                unsigned s5 = (insn >> 20) & 31;
                if (f3 == 1 && f6 == 0x02) {  // slli.uw
                    r = (a & 0xffffffffull) << ((insn >> 20) & 63);
                    return true;
                }
                if (f3 == 1 && f12 == 0x600) { r = clz(w, 32); return true; }
                if (f3 == 1 && f12 == 0x601) { r = ctz(w, 32); return true; }
                if (f3 == 1 && f12 == 0x602) { r = (uint64_t)__builtin_popcount(w); return true; }
                if (f3 == 5 && f7 == 0x30) { r = (uint64_t)(int64_t)(int32_t)(uint32_t)(rol(w, (32 - s5) & 31, 32)); return true; }  // roriw
                return false;
            }
            case 0x3b: {  // RV64 only
                uint32_t wa = (uint32_t)a;
                if (f7 == 0x04 && f3 == 0) { r = (a & 0xffffffffull) + b; return true; }       // add.uw
                if (f7 == 0x04 && f3 == 4) { r = (uint64_t)(int64_t)(int32_t)(uint32_t)((a & 0xffff) | ((b & 0xffff) << 16)); return true; }  // packw; zext.h when rs2 = x0
                if (f7 == 0x10 && (f3 == 2 || f3 == 4 || f3 == 6)) {                         // shNadd.uw
                    r = ((a & 0xffffffffull) << (f3 >> 1)) + b;
                    return true;
                }
                if (f7 == 0x30 && f3 == 1) { r = (uint64_t)(int64_t)(int32_t)(uint32_t)(rol(wa, (unsigned)b & 31, 32)); return true; }                   // rolw
                if (f7 == 0x30 && f3 == 5) { r = (uint64_t)(int64_t)(int32_t)(uint32_t)(rol(wa, (32 - ((unsigned)b & 31)) & 31, 32)); return true; }     // rorw
                return false;
            }
        }
        return false;
    }

    static constexpr uint64_t MISA_EXT =
        (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 8) | (1u << 12) | (1u << 18) | (1u << 20) |
        (1u << 21);
    // mstatus.SD: set when the FP or vector state is dirty (FS or VS = 3); bit XLEN-1.
    static uint64_t sd(const Hart& h) {
        return ((h.mstatus & MSTATUS_FS) == MSTATUS_FS || (h.mstatus & MSTATUS_VS) == MSTATUS_VS)
                   ? (h.rv64 ? 1ull << 63 : 1ull << 31) : 0;
    }
    // UXL and SXL in mstatus (sstatus has UXL): XLEN 64, fixed.
    static constexpr uint64_t MSTATUS_XL64 = (2ull << 32) | (2ull << 34);
    bool csrRead(Hart& h, unsigned csr, uint64_t& v) {
        if (((csr >> 8) & 3) > h.priv) return false;
        auto counter = [&](unsigned bit) {
            return h.priv == 3 || (((h.mcounteren >> bit) & 1) && (h.priv == 1 || ((h.scounteren >> bit) & 1)));
        };
        const uint64_t cyc = h.cycle + h.cycleAdj, ins = h.instret + h.instretAdj;
        // The upper halves of 64-bit CSRs exist on RV32 only.
        if (h.rv64 && (csr == 0x310 || csr == 0x31a || csr == 0x757 || (csr >= 0xb80 && csr <= 0xb9f) ||
                       (csr >= 0xc80 && csr <= 0xc9f)))
            return false;
        switch (csr) {
            case 0x300: v = h.mstatus | (h.rv64 ? MSTATUS_XL64 : 0) | sd(h); return true;
            case 0x001: if (!(h.mstatus & MSTATUS_FS)) return false; v = h.fcsr & 0x1f; return true;
            case 0x002: if (!(h.mstatus & MSTATUS_FS)) return false; v = (h.fcsr >> 5) & 7; return true;
            case 0x003: if (!(h.mstatus & MSTATUS_FS)) return false; v = h.fcsr & 0xff; return true;
            // V: vstart, vxsat, vxrm, vcsr, vl, vtype, vlenb.
            case 0x008: if (!(h.mstatus & MSTATUS_VS)) return false; v = h.vstart; return true;
            case 0x009: if (!(h.mstatus & MSTATUS_VS)) return false; v = h.vxsat; return true;
            case 0x00a: if (!(h.mstatus & MSTATUS_VS)) return false; v = h.vxrm; return true;
            case 0x00f: if (!(h.mstatus & MSTATUS_VS)) return false; v = (uint64_t)h.vxrm << 1 | h.vxsat; return true;
            case 0xc20: if (!(h.mstatus & MSTATUS_VS)) return false; v = h.vl; return true;
            case 0xc21:
                if (!(h.mstatus & MSTATUS_VS)) return false;
                v = h.vill ? (h.rv64 ? 1ull << 63 : 1ull << 31) : h.vtypeRaw;
                return true;
            case 0xc22: if (!(h.mstatus & MSTATUS_VS)) return false; v = h.vlen / 8; return true;
            case 0x301: v = (h.rv64 ? 2ull << 62 : 1ull << 30) | MISA_EXT; return true;
            case 0x302: v = h.medeleg; return true;
            case 0x303: v = h.mideleg; return true;
            case 0x100: v = (h.mstatus & SSTATUS_MASK) | (h.rv64 ? 2ull << 32 : 0) | sd(h); return true;
            case 0x104: v = h.mie & h.mideleg; return true;
            case 0x105: v = h.stvec; return true;
            case 0x106: v = h.scounteren; return true;
            case 0x10a: v = 0x80; return true;  // senvcfg: CBZE
            case 0x140: v = h.sscratch; return true;
            case 0x141: v = h.sepc; return true;
            case 0x142: v = h.scause; return true;
            case 0x143: v = h.stval; return true;
            case 0x144: v = h.mip & h.mideleg; return true;
            case 0x180: if (h.priv == 1 && (h.mstatus & MSTATUS_TVM)) return false; v = h.satp; return true;
            case 0x304: v = h.mie; return true;
            case 0x305: v = h.mtvec; return true;
            case 0x306: v = h.mcounteren; return true;
            case 0x310: v = 0; return true;  // mstatush
            case 0x320: v = h.mcountinhibit; return true;
            case 0x340: v = h.mscratch; return true;
            case 0x341: v = h.mepc; return true;
            case 0x342: v = h.mcause; return true;
            case 0x343: v = h.mtval; return true;
            case 0x344: v = h.mip; return true;
            case 0x30a: v = 0x80 | (h.pbmte ? 1ull << 62 : 0); return true;  // menvcfg: CBZE (CBO.ZERO below M), PBMTE
            case 0x31a: v = 0; return true;  // menvcfgh
            case 0x747: case 0x757: v = 0; return true;  // mseccfg
            // Sdtrig with no triggers: tselect is hard-wired to 0 and tdata1 says
            // "no trigger" (type 0), which is how software finds there are none.
            case 0x7a0: case 0x7a1: case 0x7a2: case 0x7a3: case 0x7a5: v = 0; return true;
            case 0x7a4: v = 1; return true;  // tinfo: type 0 only
            case 0xf11: case 0xf12: case 0xf13: case 0xf15: v = 0; return true;
            case 0xf14: v = h.id; return true;
            case 0xb00: v = cyc; return true;
            case 0xb80: v = cyc >> 32; return true;
            case 0xb02: v = ins; return true;
            case 0xb82: v = ins >> 32; return true;
            case 0xc00: if (!counter(0)) return false; v = cyc; return true;
            case 0xc80: if (!counter(0)) return false; v = cyc >> 32; return true;
            case 0xc01: if (!counter(1)) return false; v = mtime(h); return true;
            case 0xc81: if (!counter(1)) return false; v = mtime(h) >> 32; return true;
            case 0xc02: if (!counter(2)) return false; v = ins; return true;
            case 0xc82: if (!counter(2)) return false; v = ins >> 32; return true;
        }
        if (csr >= 0x3a0 && csr <= 0x3a3) {
            // pmpcfg0..3 on RV32 (4 entries each); pmpcfg0 and 2 on RV64 (8 each).
            if (h.rv64 && (csr & 1)) return false;
            unsigned n = h.rv64 ? 8 : 4, first = (csr - 0x3a0) * 4;
            v = 0;
            for (unsigned i = 0; i < n; i++) v |= (uint64_t)h.pmpcfg[first + i] << (8 * i);
            return true;
        }
        if (csr >= 0x3b0 && csr <= 0x3bf) { v = h.pmpaddr[csr - 0x3b0]; return true; }
        if ((csr >= 0xb03 && csr <= 0xb1f) || (csr >= 0xb83 && csr <= 0xb9f) || (csr >= 0x323 && csr <= 0x33f)) { v = 0; return true; }
        if ((csr >= 0xc03 && csr <= 0xc1f) || (csr >= 0xc83 && csr <= 0xc9f)) {
            if (!counter(csr & 31)) return false;
            v = 0;
            return true;
        }
        return false;
    }
    // `v` is as wide as the hart's XLEN (zero above bit 31 on RV32).
    bool csrWrite(Hart& h, unsigned csr, uint64_t v) {
        if (((csr >> 8) & 3) > h.priv || (csr >> 10) == 3) return false;
        if (h.rv64 && (csr == 0x310 || csr == 0x31a || csr == 0x757 || (csr >= 0xb80 && csr <= 0xb9f))) return false;
        switch (csr) {
            case 0x300: {
                uint64_t mpp = (v >> 11) & 3;
                if (mpp == 2) mpp = h.mstatus >> 11 & 3;  // WARL: M, S or U
                h.mstatus = (v & (MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPRV | MSTATUS_TW | MSTATUS_SIE | MSTATUS_SPIE |
                                  MSTATUS_SPP | MSTATUS_SUM | MSTATUS_MXR | MSTATUS_TVM | MSTATUS_TSR | MSTATUS_FS |
                                  MSTATUS_VS)) |
                            mpp << 11;
                return true;
            }
            case 0x001: if (!(h.mstatus & MSTATUS_FS)) return false; h.fcsr = (h.fcsr & ~0x1fu) | (v & 0x1f); h.mstatus |= MSTATUS_FS; return true;
            case 0x002: if (!(h.mstatus & MSTATUS_FS)) return false; h.fcsr = (h.fcsr & 0x1f) | (v & 7) << 5; h.mstatus |= MSTATUS_FS; return true;
            case 0x003: if (!(h.mstatus & MSTATUS_FS)) return false; h.fcsr = v & 0xff; h.mstatus |= MSTATUS_FS; return true;
            case 0x008: if (!(h.mstatus & MSTATUS_VS)) return false; h.vstart = v & (h.vlen - 1); h.mstatus |= MSTATUS_VS; return true;
            case 0x009: if (!(h.mstatus & MSTATUS_VS)) return false; h.vxsat = v & 1; h.mstatus |= MSTATUS_VS; return true;
            case 0x00a: if (!(h.mstatus & MSTATUS_VS)) return false; h.vxrm = v & 3; h.mstatus |= MSTATUS_VS; return true;
            case 0x00f:
                if (!(h.mstatus & MSTATUS_VS)) return false;
                h.vxsat = v & 1; h.vxrm = (v >> 1) & 3; h.mstatus |= MSTATUS_VS;
                return true;
            case 0x301: return true;  // misa: fixed
            case 0x304: h.mie = (uint32_t)v & (MIP_MSIP | MIP_MTIP | MIP_MEIP | MIP_SSIP | MIP_STIP | MIP_SEIP); if (h.wfi && (h.mip & h.mie)) h.wfi = false; return true;
            case 0x302: h.medeleg = (uint32_t)v & 0xb3ffu; return true;  // not ecall from M
            case 0x303: h.mideleg = (uint32_t)v & (MIP_SSIP | MIP_STIP | MIP_SEIP); return true;
            case 0x100: h.mstatus = (h.mstatus & ~(uint64_t)SSTATUS_MASK) | (v & SSTATUS_MASK); return true;
            case 0x104: h.mie = (h.mie & ~h.mideleg) | ((uint32_t)v & h.mideleg); if (h.wfi && (h.mip & h.mie)) h.wfi = false; return true;
            case 0x105: h.stvec = v & ~2ull; return true;
            case 0x106: h.scounteren = (uint32_t)v & 7; return true;
            case 0x10a: return true;
            case 0x140: h.sscratch = v; return true;
            case 0x141: h.sepc = v & ~1ull; return true;
            case 0x142: h.scause = v; return true;
            case 0x143: h.stval = v; return true;
            case 0x144: setMipSw(h, (h.mipSw & ~(h.mideleg & MIP_SSIP)) | ((uint32_t)v & h.mideleg & MIP_SSIP)); return true;
            case 0x180:
                if (h.priv == 1 && (h.mstatus & MSTATUS_TVM)) return false;
                if (h.rv64) {
                    uint64_t mode = v >> 60;
                    if (mode != 0 && (mode < 8 || mode > 10)) return true;  // not Bare, Sv39, Sv48 or Sv57: no effect
                    h.satp = (mode << 60) | (v & ((1ull << 44) - 1));  // MODE, PPN; no ASIDs
                } else {
                    h.satp = v & 0x803fffffu;  // MODE, PPN; no ASIDs
                }
                h.flushTlb();  // the decoded cache and blocks go by physical address
                return true;
            case 0x305: h.mtvec = v & ~2ull; return true;
            case 0x306: h.mcounteren = (uint32_t)v & 7; return true;
            case 0x310: return true;
            case 0x320: h.mcountinhibit = (uint32_t)v & 5; return true;
            case 0x340: h.mscratch = v; return true;
            case 0x341: h.mepc = v & ~1ull; return true;
            case 0x342: h.mcause = v; return true;
            case 0x343: h.mtval = v; return true;
            case 0x344: setMipSw(h, (uint32_t)v & (MIP_SSIP | MIP_STIP | MIP_SEIP)); return true;  // the M bits are read-only
            case 0x30a:
                if (h.rv64 && h.pbmte != ((v >> 62) & 1)) { h.pbmte = (v >> 62) & 1; h.flushTlb(); }
                return true;
            case 0x31a: case 0x747: case 0x757: return true;
            case 0x7a0: case 0x7a1: case 0x7a2: case 0x7a3: case 0x7a5: return true;
            case 0x7a4: return false;
            case 0xb00:
                if (h.rv64) h.cycleAdj = (int64_t)(v - h.cycle);
                else h.cycleAdj = (int64_t)((((h.cycle + h.cycleAdj) & ~0xffffffffull) | (uint32_t)v) - h.cycle);
                return true;
            case 0xb80: h.cycleAdj = (int64_t)((((h.cycle + h.cycleAdj) & 0xffffffffull) | v << 32) - h.cycle); return true;
            case 0xb02:
                if (h.rv64) h.instretAdj = (int64_t)(v - h.instret - 1);
                else h.instretAdj = (int64_t)((((h.instret + h.instretAdj) & ~0xffffffffull) | (uint32_t)v) - h.instret - 1);
                return true;
            case 0xb82: h.instretAdj = (int64_t)((((h.instret + h.instretAdj) & 0xffffffffull) | v << 32) - h.instret - 1); return true;
        }
        if (csr >= 0x3a0 && csr <= 0x3a3) {
            if (h.rv64 && (csr & 1)) return false;
            unsigned n = h.rv64 ? 8 : 4, first = (csr - 0x3a0) * 4;
            for (unsigned i = 0; i < n; i++) {
                uint8_t& c = h.pmpcfg[first + i];
                uint8_t b = (uint8_t)(v >> (8 * i));
                if (c & PMP_L) continue;  // locked: writes ignored
                b &= PMP_R | PMP_W | PMP_X | PMP_A | PMP_L;
                if ((b & (PMP_R | PMP_W)) == PMP_W) b &= ~PMP_W;  // W without R is reserved
                c = b;
            }
            pmpDecode(h);
            return true;
        }
        if (csr >= 0x3b0 && csr <= 0x3bf) {
            unsigned i = csr - 0x3b0;
            if (pmpCfg(h, i) & PMP_L) return true;
            if (i < 15 && (pmpCfg(h, i + 1) & (PMP_L | PMP_A)) == (PMP_L | PMP_TOR)) return true;
            h.pmpaddr[i] = h.rv64 ? v & ((1ull << 54) - 1) : (uint32_t)v;
            pmpDecode(h);
            return true;
        }
        if ((csr >= 0xb03 && csr <= 0xb1f) || (csr >= 0xb83 && csr <= 0xb9f) || (csr >= 0x323 && csr <= 0x33f)) return true;
        return false;
    }

    // ------------------------------------------------------------ PMP
    // Physical memory protection, 16 entries, grain 4 bytes (Privileged
    // Architecture 3.7). Checked only when it can refuse something: below
    // machine mode (or with MPRV for loads and stores), or once an entry is
    // locked; so machine-mode code without locks keeps the fast path.
    enum : uint8_t { PMP_R = 1, PMP_W = 2, PMP_X = 4, PMP_A = 0x18, PMP_TOR = 0x08, PMP_NA4 = 0x10,
                     PMP_NAPOT = 0x18, PMP_L = 0x80 };
    static uint8_t pmpCfg(const Hart& h, unsigned i) { return h.pmpcfg[i]; }
    static void pmpDecode(Hart& h) {
        h.pmpN = 0;
        h.fpVpn = ~0ull;
        h.flushFetchPages();
        h.flushDataPages();
        h.pmpLock = false;
        for (unsigned i = 0; i < 16; i++) {
            uint8_t c = pmpCfg(h, i);
            if (!(c & PMP_A)) continue;
            uint64_t a = (uint64_t)h.pmpaddr[i] << 2, lo, hi;
            if ((c & PMP_A) == PMP_TOR) { lo = i ? (uint64_t)h.pmpaddr[i - 1] << 2 : 0; hi = a; }
            else if ((c & PMP_A) == PMP_NA4) { lo = a; hi = a + 4; }
            else {
                unsigned t = 0;
                while (t < 62 && ((h.pmpaddr[i] >> t) & 1)) t++;
                uint64_t size = 8ull << t;
                lo = a & ~(size - 1);
                hi = lo + size;
            }
            if (c & PMP_L) h.pmpLock = true;
            h.pmpR[h.pmpN++] = {lo, hi, c};
        }
    }
    // Is an access of `n` bytes at `a` with permission `perm` allowed?
    static bool pmpAllows(const Hart& h, uint64_t a, unsigned n, uint8_t perm, unsigned priv) {
        uint64_t lo = a, hi = (uint64_t)a + n;
        for (unsigned i = 0; i < h.pmpN; i++) {
            const Hart::PmpRegion& r = h.pmpR[i];
            if (hi <= r.lo || lo >= r.hi) continue;
            if (lo < r.lo || hi > r.hi) return false;  // only part of the access matches
            if (priv == 3 && !(r.cfg & PMP_L)) return true;
            return (r.cfg & perm) != 0;
        }
        return priv == 3;
    }
    // Privilege that loads and stores are checked at.
    static unsigned dataPriv(const Hart& h) {
        return (h.mstatus & MSTATUS_MPRV) ? (h.mstatus >> 11) & 3 : h.priv;
    }
    static bool pmpData(const Hart& h, uint64_t a, unsigned n, uint8_t perm) {
        unsigned p = dataPriv(h);
        if (p == 3 && !h.pmpLock) return true;
        return pmpAllows(h, a, n, perm, p);
    }

    // ------------------------------------------------------------ traps
    void setMipSw(Hart& h, uint32_t v) {
        h.mipSw = v;
        h.mip = (h.mip & ~(MIP_SSIP | MIP_STIP | MIP_SEIP)) | v |
                ((plic && plic->eip(2 * h.id + 1)) ? MIP_SEIP : 0u);
        if (h.wfi && (h.mip & h.mie)) h.wfi = false;
    }
    void trap(Hart& h, uint32_t cause, uint64_t tval, bool intr) {
        bool toS = h.priv <= 1 && (((intr ? h.mideleg : h.medeleg) >> cause) & 1);
        const uint64_t ibit = intr ? (h.rv64 ? 1ull << 63 : 1ull << 31) : 0;
        tval = h.ax(tval);
        uint64_t s = h.mstatus;
        if (toS) {
            h.sepc = h.pc;
            h.scause = cause | ibit;
            h.stval = tval;
            s = (s & ~(uint64_t)MSTATUS_SPIE) | ((s & MSTATUS_SIE) ? (uint64_t)MSTATUS_SPIE : 0);
            s &= ~(uint64_t)MSTATUS_SIE;
            s = (s & ~(uint64_t)MSTATUS_SPP) | (h.priv ? (uint64_t)MSTATUS_SPP : 0);
            h.mstatus = s;
            h.priv = 1;
            h.pc = h.ax((h.stvec & ~3ull) + ((h.stvec & 1) && intr ? 4 * cause : 0));
        } else {
            h.mepc = h.pc;
            h.mcause = cause | ibit;
            h.mtval = tval;
            s = (s & ~(uint64_t)MSTATUS_MPIE) | ((s & MSTATUS_MIE) ? (uint64_t)MSTATUS_MPIE : 0);
            s &= ~(uint64_t)MSTATUS_MIE;
            s = (s & ~(uint64_t)MSTATUS_MPP) | (uint64_t)h.priv << 11;
            h.mstatus = s;
            h.priv = 3;
            h.pc = h.ax((h.mtvec & ~3ull) + ((h.mtvec & 1) && intr ? 4 * cause : 0));
        }
        h.resv = false;
        if (trace) {
            char b[128];
            int w = h.rv64 ? 16 : 8;
            snprintf(b, sizeof b, "core %3u: exception %s%u, epc 0x%0*llx, tval 0x%0*llx\n", h.id,
                     intr ? "interrupt " : "", cause, w, (unsigned long long)(toS ? h.sepc : h.mepc), w,
                     (unsigned long long)tval);
            h.traceBuf += b;
        }
    }
    // Take the highest-priority enabled pending interrupt, if any.
    bool interrupt(Hart& h) {
        uint32_t pend = h.mip & h.mie;
        if (!pend) return false;
        // Taken in M unless delegated; in S only from S with SIE, or from U.
        bool mOn = h.priv < 3 || (h.mstatus & MSTATUS_MIE);
        bool sOn = h.priv < 1 || (h.priv == 1 && (h.mstatus & MSTATUS_SIE));
        uint32_t m = mOn ? pend & ~h.mideleg : 0, sv = sOn ? pend & h.mideleg : 0;
        uint32_t take = m ? m : sv;
        if (!take) return false;
        static const uint32_t order[] = {11, 3, 7, 9, 1, 5};  // MEI MSI MTI SEI SSI STI
        for (uint32_t c : order)
            if ((take >> c) & 1) { trap(h, c, 0, true); return true; }
        return false;
    }

    // ---------------------------------------------- Sv32, Sv39, Sv48, Sv57
    // Translate `va` for an access of kind 0 read, 1 write, 2 execute at
    // privilege `priv`. Svade: a clear A, or a clear D on a write, is a page
    // fault, which software answers by setting the bit. Page-table reads are
    // checked by PMP as supervisor accesses.
    Acc physReadPte(Hart& h, uint64_t pa, unsigned n, uint64_t& v) {
        if (!((pa >= h.cBase && pa + n <= h.cEnd))) {
            Device* d = bus.find(pa);
            if (!d || !d->memory() || pa + n > d->base + d->size) return Acc::Fault;
            h.cBase = d->base; h.cEnd = d->base + d->size; h.cPtr = d->memory(); h.cRw = d->writable();
        }
        return ramRead(h, pa, n, v);
    }
    Acc translate(Hart& h, uint64_t va, int kind, unsigned priv, uint64_t& pa) {
        if (priv == 3 || !h.paging()) { pa = va; return Acc::Ok; }
        // Sv32 has 2 levels of 10-bit indices and 4-byte entries with a 22-bit
        // PPN; Sv39, Sv48 and Sv57 (satp.MODE 8, 9, 10) have 3, 4 and 5
        // levels of 9 bits, 8-byte entries and a 44-bit PPN.
        const bool w = h.rv64;
        const unsigned levels = w ? (unsigned)(h.satp >> 60) - 5 : 2, ib = w ? 9 : 10, pteN = w ? 8 : 4;
        const unsigned vab = 12 + 9 * levels;  // 39, 48, 57
        if (w && (uint64_t)((int64_t)(va << (64 - vab)) >> (64 - vab)) != va) return Acc::PageFault;  // not sign-extended
        const uint64_t ppnMask = w ? (1ull << 44) - 1 : 0x3fffff;
        uint64_t vpn = w ? (va >> 12) & ((1ull << (9 * levels)) - 1) : (va >> 12) & 0xfffff;
        Hart::Tlb& t = h.tlb[vpn & (Hart::TLB_SIZE - 1)];
        for (int pass = 0; pass < 2; pass++) {
            if (!(t.valid && t.vpn == vpn)) {
                // Walk the table.
                uint64_t a = (h.satp & ppnMask) << 12, pte = 0;
                int i = (int)levels - 1;
                for (;; i--) {
                    uint64_t pteA = a + ((vpn >> (ib * i)) & ((1u << ib) - 1)) * pteN;
                    if (!pmpAllows(h, pteA, pteN, PMP_R, 1)) return Acc::Fault;
                    if (physReadPte(h, pteA, pteN, pte) != Acc::Ok) return Acc::Fault;
                    if (!(pte & 1) || ((pte & 6) == 4)) return Acc::PageFault;  // invalid, or W without R
                    if (w && ((pte >> 54) & 0x7f)) return Acc::PageFault;       // reserved
                    if (pte & 0xa) {                                              // R or X: a leaf
                        // Svpbmt: PMA, NC or IO with menvcfg.PBMTE (all the
                        // same here: no caches), 3 reserved.
                        const unsigned pbmt = w ? (unsigned)(pte >> 61) & 3 : 0;
                        if (pbmt == 3 || (pbmt && !h.pbmte)) return Acc::PageFault;
                        break;
                    }
                    if (w && ((pte >> 61) & 3)) return Acc::PageFault;           // PBMT on a pointer
                    if (pte & 0xd0) return Acc::PageFault;                        // D, A, U on a pointer
                    if (w && (pte >> 63)) return Acc::PageFault;                  // N on a pointer
                    if (i == 0) return Acc::PageFault;
                    a = ((pte >> 10) & ppnMask) << 12;
                }
                uint64_t ppn = (pte >> 10) & ppnMask;
                if (w && (pte >> 63)) {                                           // Svnapot: a 64 KiB page
                    if (i != 0 || (ppn & 0xf) != 8) return Acc::PageFault;
                    ppn = (ppn & ~0xfull) | (vpn & 0xf);
                }
                if (i > 0) {
                    uint64_t low = (1ull << (ib * i)) - 1;
                    if (ppn & low) return Acc::PageFault;  // misaligned superpage
                    ppn |= vpn & low;
                }
                t.valid = true; t.vpn = vpn; t.ppn = ppn; t.pte = (uint8_t)pte;
                if (i > 0) h.noteSuper(va, 12 + ib * (unsigned)i);
                else if (w && (pte >> 63)) h.noteSuper(va, 16);
            }
            uint8_t f = t.pte;
            bool ok = true;
            if (f & 0x10) {                                                       // U page
                if (priv == 1 && (kind == 2 || !(h.mstatus & MSTATUS_SUM))) ok = false;
            } else if (priv == 0) ok = false;
            if (kind == 0 && !((f & 2) || ((h.mstatus & MSTATUS_MXR) && (f & 8)))) ok = false;
            if (kind == 1 && !(f & 4)) ok = false;
            if (kind == 2 && !(f & 8)) ok = false;
            bool ad = (f & 0x40) && (kind != 1 || (f & 0x80));
            if (ok && ad) { pa = (t.ppn << 12) | (va & 0xfff); return Acc::Ok; }
            if (ok && !ad && pass == 0) { t.valid = false; continue; }  // software may have set A/D since
            return Acc::PageFault;
        }
        return Acc::PageFault;
    }
    bool dataXlat(const Hart& h) const { return h.paging() && dataPriv(h) != 3; }
    // A data access of `n` bytes at virtual `va`, split where it crosses a page.
    template <class F>
    Acc xlatData(Hart& h, uint64_t va, unsigned n, int kind, F&& f) {
        unsigned priv = dataPriv(h);
        if (((va & 0xfff) + n) <= 4096) {
            uint64_t pa;
            Acc r = translate(h, va, kind, priv, pa);
            if (r != Acc::Ok) return r;
            return f(pa, 0u, n);
        }
        unsigned first = 4096 - (va & 0xfff);
        uint64_t pa0, pa1;
        Acc r = translate(h, va, kind, priv, pa0);
        if (r != Acc::Ok) return r;
        r = translate(h, h.ax(va + first), kind, priv, pa1);
        if (r != Acc::Ok) return r;
        r = f(pa0, 0u, first);
        if (r != Acc::Ok) return r;
        return f(pa1, first, n - first);
    }

    // ------------------------------------------------------------ memory
    // Accesses are of 1, 2, 4 or 8 bytes, at physical addresses below 2^56
    // and virtual ones as the hart's XLEN makes them.
    // The device side of an access that missed the fast path.
    Acc devRead(Hart& h, uint64_t a, unsigned n, uint64_t& v) {
        Device* d = bus.find(a);
        if (!d || a + n > d->base + d->size) return Acc::Fault;
        if (d->memory()) {
            h.cBase = d->base; h.cEnd = d->base + d->size; h.cPtr = d->memory(); h.cRw = d->writable();
            return ramRead(h, a, n, v);
        }
        if (h.par) return Acc::Pending;
        if (merge_) cutAfter(h);
        uint64_t t = 0;
        Who w{h.id, h.cycle, true};
        Acc r = d->read(a - d->base, n, t, w);
        if (r == Acc::Ok) { v = t; d->nread++; }
        return r;
    }
    Acc devWrite(Hart& h, uint64_t a, unsigned n, uint64_t v) {
        Device* d = bus.find(a);
        if (!d || a + n > d->base + d->size) return Acc::Fault;
        if (d->memory()) {
            h.cBase = d->base; h.cEnd = d->base + d->size; h.cPtr = d->memory(); h.cRw = d->writable();
            if (!h.cRw) return Acc::Fault;
            return ramWrite(h, a, n, v);
        }
        if (h.par) return Acc::Pending;
        if (merge_) cutAfter(h);
        Who w{h.id, h.cycle, true};
        if (n < 8) v &= (1ull << (8 * n)) - 1;
        Acc r = d->write(a - d->base, n, v, w);
        if (r == Acc::Ok) d->nwrite++;
        return r;
    }
    // A device was touched in merged quanta: stop at the end of this quantum.
    void cutAfter(const Hart& h) {
        const uint64_t b = gcycle_ + ((h.cycle - gcycle_) / quantum + 1) * quantum;
        if (b < cut_) cut_ = b;
    }
    // Plain memory, through the hart's write buffer in a parallel round.
    Acc ramRead(Hart& h, uint64_t a, unsigned n, uint64_t& v) {
        const uint8_t* p = h.cPtr + (a - h.cBase);
        uint64_t t = 0;
        if (!h.par || h.ov.empty()) {
            memcpy(&t, p, n);
            v = t;
            return Acc::Ok;
        }
        if (((a & 4095) + n) <= 4096) {
            Overlay::Page* pg = h.ov.find(a >> 12);
            if (!pg) { memcpy(&t, p, n); v = t; return Acc::Ok; }
            unsigned o = a & 4095;
            for (unsigned i = 0; i < n; i++)
                t |= (uint64_t)(pg->has(o + i) ? pg->d[o + i] : p[i]) << (8 * i);
            v = t;
            return Acc::Ok;
        }
        for (unsigned i = 0; i < n; i++) {
            uint64_t ai = a + i;
            Overlay::Page* pg = h.ov.find(ai >> 12);
            uint8_t b = (pg && pg->has(ai & 4095)) ? pg->d[ai & 4095] : p[i];
            t |= (uint64_t)b << (8 * i);
        }
        v = t;
        return Acc::Ok;
    }
    Acc ramWrite(Hart& h, uint64_t a, unsigned n, uint64_t v) {
        if (tohost && a + n > tohost && a < tohost + 8) {
            if (h.par) return Acc::Pending;
            memcpy(h.cPtr + (a - h.cBase), &v, n);
            htif(h);
            return Acc::Ok;
        }
        if (!h.par) {
            memcpy(h.cPtr + (a - h.cBase), &v, n);
            return Acc::Ok;
        }
        if (((a & 4095) + n) <= 4096) {
            Overlay::Page* pg = h.ov.touch(a >> 12, h.cPtr + ((a & ~4095ull) - h.cBase));
            unsigned o = a & 4095;
            memcpy(pg->d + o, &v, n);
            for (unsigned i = 0; i < n; i++) pg->set(o + i);
            return Acc::Ok;
        }
        for (unsigned i = 0; i < n; i++) {
            uint64_t ai = a + i;
            uint8_t* base = h.cPtr + ((ai & ~4095ull) - h.cBase);
            Overlay::Page* pg = h.ov.touch(ai >> 12, base);
            pg->d[ai & 4095] = (uint8_t)(v >> (8 * i));
            pg->set(ai & 4095);
        }
        return Acc::Ok;
    }
    // The hot path of a load or store: plain memory, nothing to check, no
    // write buffer in use. Everything else goes through loadSlow/storeSlow.
    __attribute__((always_inline)) inline Acc load(Hart& h, uint64_t a, unsigned n, uint64_t& v) {
        if (__builtin_expect(watches.empty() && !(h.priv != 3 || h.pmpLock || (h.mstatus & MSTATUS_MPRV)) &&
                             a >= h.cBase && a + n <= h.cEnd && (!h.par || h.ov.empty()), 1)) {
            uint64_t t = 0;
            memcpy(&t, h.cPtr + (a - h.cBase), n);
            v = t;
            return Acc::Ok;
        }
        return loadSlow(h, a, n, v);
    }
    __attribute__((always_inline)) inline Acc store(Hart& h, uint64_t a, unsigned n, uint64_t v) {
        if (__builtin_expect(watches.empty() && !(h.priv != 3 || h.pmpLock || (h.mstatus & MSTATUS_MPRV)) &&
                             a >= h.cBase && a + n <= h.cEnd && h.cRw && !h.par &&
                             !(tohost && a + n > tohost && a < tohost + 8), 1)) {
            memcpy(h.cPtr + (a - h.cBase), &v, n);
            return Acc::Ok;
        }
        return storeSlow(h, a, n, v);
    }
    // The key a data page was checked under: privilege, SUM, MXR, and the
    // page caches' epoch.
    static uint32_t dataCtx(const Hart& h) {
        return dataPriv(h) | (h.mstatus & MSTATUS_SUM ? 4 : 0) | (h.mstatus & MSTATUS_MXR ? 8 : 0) | h.dpEpoch << 4;
    }
    // With paging on: an access within one page through the data page cache,
    // or a translation that fills it when the page is plain memory that PMP
    // allows as a whole. Returns false when the access has to take the
    // general path (a device, part of a page allowed, a write buffer).
    bool dataPage(Hart& h, uint64_t a, bool st, uint8_t*& p, Acc& r) {
        uint32_t ctx = dataCtx(h);
        Hart::DataPage& e = (st ? h.ds : h.dl)[(a >> 12) & (Hart::DP_SIZE - 1)];
        const uint64_t tag = Hart::dpTag(a, ctx);
        if (e.tag == tag) { p = e.host + (a & 0xfff); return true; }
        uint64_t pa;
        r = translate(h, a, st ? 1 : 0, dataPriv(h), pa);
        if (r != Acc::Ok) { p = nullptr; return true; }
        uint64_t pg = pa & ~0xfffull;
        Device* d = bus.find(pg);
        if (!d || !d->memory() || pg + 4096 > d->base + d->size || (st && !d->writable()) ||
            (tohost && (tohost >> 12) == (pa >> 12)) || !pmpAllows(h, pg, 4096, st ? PMP_W : PMP_R, dataPriv(h)))
            return false;
        e.tag = tag;
        e.host = d->memory() + (pg - d->base);
        p = e.host + (a & 0xfff);
        return true;
    }
    __attribute__((noinline)) Acc loadSlow(Hart& h, uint64_t a, unsigned n, uint64_t& v) {
        if (!watches.empty()) watchCheck(h, a, n, 3);
        if (watches.empty() && (!h.par || h.ov.empty()) && ((a & 0xfff) + n) <= 4096 && dataXlat(h)) {
            uint8_t* p;
            Acc r = Acc::Ok;
            if (dataPage(h, a, false, p, r)) {
                if (!p) return r;
                uint64_t t = 0;
                memcpy(&t, p, n);
                v = t;
                return Acc::Ok;
            }
        }
        if (dataXlat(h)) {
            uint64_t out = 0;
            Acc r = xlatData(h, a, n, 0, [&](uint64_t pa, unsigned at, unsigned k) {
                uint64_t part = 0;
                Acc q = physLoad(h, pa, k, part);
                out |= part << (8 * at);
                return q;
            });
            if (r == Acc::Ok) v = out;
            return r;
        }
        return physLoad(h, a, n, v);
    }
    __attribute__((noinline)) Acc storeSlow(Hart& h, uint64_t a, unsigned n, uint64_t v) {
        if (!watches.empty()) watchCheck(h, a, n, 2);
        if (watches.empty() && !h.par && ((a & 0xfff) + n) <= 4096 && dataXlat(h)) {
            uint8_t* p;
            Acc r = Acc::Ok;
            if (dataPage(h, a, true, p, r)) {
                if (!p) return r;
                memcpy(p, &v, n);
                return Acc::Ok;
            }
        }
        if (dataXlat(h))
            return xlatData(h, a, n, 1, [&](uint64_t pa, unsigned at, unsigned k) {
                return physStore(h, pa, k, at < 8 ? v >> (8 * at) : 0);
            });
        return physStore(h, a, n, v);
    }
    Acc physLoad(Hart& h, uint64_t a, unsigned n, uint64_t& v) {
        if ((h.priv != 3 || h.pmpLock || (h.mstatus & MSTATUS_MPRV)) && !pmpData(h, a, n, PMP_R)) return Acc::Fault;
        if (a >= h.cBase && a + n <= h.cEnd) return ramRead(h, a, n, v);
        return devRead(h, a, n, v);
    }
    Acc physStore(Hart& h, uint64_t a, unsigned n, uint64_t v) {
        if ((h.priv != 3 || h.pmpLock || (h.mstatus & MSTATUS_MPRV)) && !pmpData(h, a, n, PMP_W)) return Acc::Fault;
        if (a >= h.cBase && a + n <= h.cEnd && h.cRw) return ramWrite(h, a, n, v);
        return devWrite(h, a, n, v);
    }
    // An AMO needs write permission for its read too.
    Acc amoCheck(Hart& h, uint64_t a) {
        if (!dataXlat(h)) return Acc::Ok;
        uint64_t pa;
        return translate(h, a, 1, dataPriv(h), pa);
    }
    // Fetch through the MMU: the instruction's bits and the trap, if any
    // (cause and tval; cause 0 = none).
    uint32_t fetchXlat(Hart& h, uint64_t pc, uint32_t& raw, uint64_t& tval, bool readBits, unsigned knownLen,
                       uint64_t* paOut = nullptr) {
        uint64_t pa;
        tval = pc;
        const bool samePage = (pc >> 12) == h.fpVpn && h.priv == h.fpPriv;
        if (samePage) {
            pa = h.fpPa | (pc & 0xfff);
            if (!readBits && (knownLen == 2 || (pc & 0xfff) <= 0xffc)) return 0;
        } else {
            Acc r = translate(h, pc, 2, h.priv, pa);
            if (r == Acc::PageFault) return 12;
            if (r != Acc::Ok || !pmpAllows(h, pa, 2, PMP_X, h.priv)) return 1;
            if (pmpAllows(h, pa & ~0xfffull, 4096, PMP_X, h.priv)) {
                h.fpVpn = pc >> 12;
                h.fpPa = pa & ~0xfffull;
                h.fpPriv = h.priv;
                h.fp[(pc >> 12) & (Hart::FP_SIZE - 1)] = {pc >> 12, h.priv, pa & ~0xfffull};
            }
        }
        if (paOut) *paOut = pa;
        Acc r;
        uint64_t lo = 0;
        if (readBits) {
            Device* d = bus.find(pa);
            if (!d || !d->memory() || pa + 2 > d->base + d->size) return 1;
            h.cBase = d->base; h.cEnd = d->base + d->size; h.cPtr = d->memory(); h.cRw = d->writable();
            ramRead(h, pa, 2, lo);
            raw = (uint32_t)lo;
        }
        unsigned len = readBits ? ((lo & 3) == 3 ? 4 : 2) : knownLen;
        if (len == 2) return 0;
        uint64_t pa2 = pa + 2;
        uint64_t pc2 = h.ax(pc + 2);
        if ((pc2 & 0xfff) == 0) {
            r = translate(h, pc2, 2, h.priv, pa2);
            tval = pc2;
            if (r == Acc::PageFault) return 12;
            if (r != Acc::Ok) return 1;
        }
        if (!pmpAllows(h, pa2, 2, PMP_X, h.priv)) { tval = pc2; return 1; }
        if (readBits) {
            Device* d = bus.find(pa2);
            if (!d || !d->memory() || pa2 + 2 > d->base + d->size) { tval = pc2; return 1; }
            h.cBase = d->base; h.cEnd = d->base + d->size; h.cPtr = d->memory(); h.cRw = d->writable();
            uint64_t hi = 0;
            ramRead(h, pa2, 2, hi);
            raw |= (uint32_t)hi << 16;
        }
        return 0;
    }
    void watchCheck(Hart& h, uint64_t a, unsigned n, int kind) {
        for (auto& w : watches)
            if (a < w.addr + w.len && w.addr < a + n && (w.kind == kind || w.kind == 4) && !h.watchKind) {
                h.watchKind = w.kind;
                h.watchAddr = a;
            }
    }
    // HTIF, as the RISC-V tests and proxy kernels use it: a write of
    // (code << 1) | 1 to tohost ends the run with `code`; device 1 command 1
    // prints the low byte.
    void htif(Hart& h) {
        Device* d = bus.find(tohost);
        uint8_t* p = d->memory() + (tohost - d->base);
        uint64_t v;
        memcpy(&v, p, 8);
        if (!v) return;
        uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
        if ((hi >> 24) == 1 && ((hi >> 16) & 0xff) == 1) {
            out(std::string(1, (char)(lo & 0xff)));
            memset(p, 0, 8);
            return;
        }
        if (lo & 1) {
            requestExit((int)(lo >> 1));
            (void)h;
        }
    }

    // ------------------------------------------------------------ the loop
    // Run hart `h` until cycle `end` or until it stops.
    Stop exec(Hart& h, uint64_t end);

private:
    // ---- The fast loop: the common integer instructions of a hart in
    // machine mode with nothing to trace, watch or buffer, straight from the
    // decoded-instruction cache. It stops before the first instruction it
    // does not handle (a cache miss, a CSR, a device access, ...), which the
    // general path then executes.
    enum : uint8_t {
        K_NONE, K_LUI, K_AUIPC, K_JAL, K_JALR, K_BEQ, K_BNE, K_BLT, K_BGE, K_BLTU, K_BGEU,
        K_LB, K_LH, K_LW, K_LD, K_LBU, K_LHU, K_LWU, K_SB, K_SH, K_SW, K_SD,
        K_ADDI, K_SLLI, K_SLTI, K_SLTIU, K_XORI, K_SRLI, K_SRAI, K_ORI, K_ANDI,
        K_ADD, K_SUB, K_SLL, K_SLT, K_SLTU, K_XOR, K_SRL, K_SRA, K_OR, K_AND, K_MUL,
        K_ADDIW, K_SLLIW, K_SRLIW, K_SRAIW, K_ADDW, K_SUBW, K_SLLW, K_SRLW, K_SRAW, K_MULW
    };
    // The fast loop's class of an instruction and its decoded immediate (a
    // shift amount for the immediate shifts); K_NONE for the general path.
    static uint8_t fastKind(uint32_t insn, bool rv64, int32_t& imm) {
        const unsigned op = insn & 0x7f, f3 = (insn >> 12) & 7, f7 = insn >> 25;
        const int32_t immI = (int32_t)insn >> 20;
        imm = immI;
        switch (op) {
            case 0x37: imm = (int32_t)(insn & 0xfffff000u); return K_LUI;
            case 0x17: imm = (int32_t)(insn & 0xfffff000u); return K_AUIPC;
            case 0x6f:
                imm = (int32_t)(((insn >> 31) << 20) | (((insn >> 12) & 0xff) << 12) | (((insn >> 20) & 1) << 11) |
                                (((insn >> 21) & 0x3ff) << 1)) << 11 >> 11;
                return K_JAL;
            case 0x67: return f3 == 0 ? K_JALR : K_NONE;
            case 0x63: {
                static const uint8_t k[8] = {K_BEQ, K_BNE, K_NONE, K_NONE, K_BLT, K_BGE, K_BLTU, K_BGEU};
                imm = (int32_t)(((insn >> 31) << 12) | (((insn >> 7) & 1) << 11) | (((insn >> 25) & 0x3f) << 5) |
                                (((insn >> 8) & 0xf) << 1)) << 19 >> 19;
                return k[f3];
            }
            case 0x03: {
                static const uint8_t k[8] = {K_LB, K_LH, K_LW, K_LD, K_LBU, K_LHU, K_LWU, K_NONE};
                return !rv64 && (f3 == 3 || f3 == 6) ? K_NONE : k[f3];
            }
            case 0x23: {
                static const uint8_t k[8] = {K_SB, K_SH, K_SW, K_SD, K_NONE, K_NONE, K_NONE, K_NONE};
                imm = (int32_t)(((insn >> 25) << 5) | ((insn >> 7) & 31)) << 20 >> 20;
                return !rv64 && f3 == 3 ? K_NONE : k[f3];
            }
            case 0x13: {
                const unsigned hi = rv64 ? insn >> 26 : f7, sra = rv64 ? 0x10 : 0x20;
                const int32_t sh = (int32_t)((insn >> 20) & (rv64 ? 63 : 31));
                switch (f3) {
                    case 0: return K_ADDI;
                    case 1: imm = sh; return hi == 0 ? K_SLLI : K_NONE;
                    case 2: return K_SLTI;
                    case 3: return K_SLTIU;
                    case 4: return K_XORI;
                    case 5: imm = sh; return hi == 0 ? K_SRLI : hi == sra ? K_SRAI : K_NONE;
                    case 6: return K_ORI;
                    default: return K_ANDI;
                }
            }
            case 0x33: {
                static const uint8_t k0[8] = {K_ADD, K_SLL, K_SLT, K_SLTU, K_XOR, K_SRL, K_OR, K_AND};
                if (f7 == 0) return k0[f3];
                if (f7 == 0x20) return f3 == 0 ? K_SUB : f3 == 5 ? K_SRA : K_NONE;
                return f7 == 1 && f3 == 0 ? K_MUL : K_NONE;
            }
            case 0x1b:
                if (!rv64) return K_NONE;
                imm = f3 == 0 ? immI : (int32_t)((insn >> 20) & 31);
                if (f3 == 0) return K_ADDIW;
                if (f3 == 1 && f7 == 0) return K_SLLIW;
                if (f3 == 5) return f7 == 0 ? K_SRLIW : f7 == 0x20 ? K_SRAIW : K_NONE;
                return K_NONE;
            case 0x3b:
                if (!rv64) return K_NONE;
                if (f7 == 0) return f3 == 0 ? K_ADDW : f3 == 1 ? K_SLLW : f3 == 5 ? K_SRLW : K_NONE;
                if (f7 == 0x20) return f3 == 0 ? K_SUBW : f3 == 5 ? K_SRAW : K_NONE;
                return f7 == 1 && f3 == 0 ? K_MULW : K_NONE;
        }
        return K_NONE;
    }
    // ---- The dynamic translator. A block of the fast loop's instructions
    // (bare RV64 machine mode for now) becomes x86-64 code; see jit.hpp for
    // the register conventions. Blocks are linked to the blocks they jump to
    // directly once those exist, and an indirect jump looks its target up in
    // a table; anything else leaves for the fast loop, which runs the
    // instruction a block could not. Translations hold for one generation of
    // the decoded-instruction cache, so FENCE.I, SFENCE.VMA and a new program
    // drop them all.
    static constexpr unsigned JIT_OPS = 64;
    // Classes only the translator has (after the fast loop's): the rest of M.
    enum : uint8_t { J_MULH = K_MULW + 1, J_MULHSU, J_MULHU, J_DIV, J_DIVU, J_REM, J_REMU, J_DIVW, J_DIVUW, J_REMW, J_REMUW, J_FENCE, J_AMO, J_CBOZ,
                     J_ZU, J_ZB, J_SSC };
    // Zbb (RV64): J_ZU reads rs1, J_ZB rs1 and rs2; the operation in imm
    // (and a rotate's amount in imm >> 8).
    enum : uint8_t { Z_CLZ = 1, Z_CTZ, Z_CPOP, Z_CLZW, Z_CTZW, Z_CPOPW, Z_SEXTB, Z_SEXTH, Z_ZEXTH, Z_ORCB, Z_REV8, Z_RORI, Z_RORIW,
                     Z_ANDN, Z_ORN, Z_XNOR, Z_MIN, Z_MINU, Z_MAX, Z_MAXU, Z_ROL, Z_ROR, Z_ROLW, Z_RORW };
    static int32_t zbbOp(uint32_t insn) {
        const unsigned op = insn & 0x7f, f3 = (insn >> 12) & 7, f7 = insn >> 25, i12 = insn >> 20;
        static const bool pop = __builtin_cpu_supports("popcnt");
        if (op == 0x13 && f3 == 1)
            return i12 == 0x600 ? Z_CLZ : i12 == 0x601 ? Z_CTZ : i12 == 0x602 && pop ? Z_CPOP : i12 == 0x604 ? Z_SEXTB : i12 == 0x605 ? Z_SEXTH : 0;
        if (op == 0x13 && f3 == 5) {
            if (i12 == 0x287) return Z_ORCB;
            if (i12 == 0x6b8) return Z_REV8;
            if ((insn >> 26) == 0x18) return Z_RORI | (int32_t)((i12 & 63) << 8);
            return 0;
        }
        if (op == 0x1b && f3 == 1) return i12 == 0x600 ? Z_CLZW : i12 == 0x601 ? Z_CTZW : i12 == 0x602 && pop ? Z_CPOPW : 0;
        if (op == 0x1b && f3 == 5 && f7 == 0x30) return Z_RORIW | (int32_t)((i12 & 31) << 8);
        if (op == 0x33 && f7 == 0x20) return f3 == 7 ? Z_ANDN : f3 == 6 ? Z_ORN : f3 == 4 ? Z_XNOR : 0;
        if (op == 0x33 && f7 == 0x05) return f3 == 4 ? Z_MIN : f3 == 5 ? Z_MINU : f3 == 6 ? Z_MAX : f3 == 7 ? Z_MAXU : 0;
        if (op == 0x33 && f7 == 0x30) return f3 == 1 ? Z_ROL : f3 == 5 ? Z_ROR : 0;
        if (op == 0x3b && f7 == 0x04 && f3 == 4 && ((insn >> 20) & 31) == 0) return Z_ZEXTH;
        if (op == 0x3b && f7 == 0x30) return f3 == 1 ? Z_ROLW : f3 == 5 ? Z_RORW : 0;
        return 0;
    }
    // pg: with paging on, where AMOs (but LR and SC; one hart only) and
    // CBO.ZERO go through the store page cache.
    static uint8_t jitKind(uint32_t insn, bool pg) {
        const unsigned op = insn & 0x7f, f3 = (insn >> 12) & 7, f7 = insn >> 25;
        // FENCE (and FENCE.TSO, PAUSE): nothing to order with one hart
        // running alone, which is when translated code runs
        if (op == 0x0f && f3 == 0) return J_FENCE;
        if (pg && op == 0x0f && f3 == 2 && (insn >> 20) == 4 && !((insn >> 7) & 31)) return J_CBOZ;
        if (op == 0x2f && (f3 == 2 || f3 == 3)) {
            switch (insn >> 27) {
                case 0x00: case 0x01: case 0x04: case 0x08: case 0x0c: case 0x10: case 0x14: case 0x18: case 0x1c:
                    return J_AMO;
                case 0x02: return ((insn >> 20) & 31) ? (uint8_t)K_NONE : (uint8_t)J_AMO;  // LR
                case 0x03: return J_AMO;                                // SC
                default: return K_NONE;
            }
        }
        if (const int32_t z = zbbOp(insn)) return (z & 0xff) < Z_ANDN ? J_ZU : J_ZB;
        if (f7 != 1) return K_NONE;
        if (op == 0x33 && f3) return (uint8_t)(J_MULH + f3 - 1);
        if (op == 0x3b && f3 >= 4) return (uint8_t)(J_DIVW + f3 - 4);
        return K_NONE;
    }
    Jit* jitFor(Hart& h) {
        if (jits_.size() <= h.id) jits_.resize(h.id + 1);
        if (!jits_[h.id]) jits_[h.id].reset(new Jit);
        return jits_[h.id]->failed ? nullptr : jits_[h.id].get();
    }
    // The translator for the hart's paged code: its own, or with -serial and
    // more than one hart one for all (they run on one thread, and the
    // kernel's code is the same for each).
    Jit* jitForPg(Hart& h) {
        Jit* v = jitFor(h);
        if (!v || !serial || harts.size() < 2) return v;
        if (!pgShared_) {
            pgShared_.reset(new Jit);
            if (pgShared_->failed) return nullptr;
        }
        if (std::find(pgShared_->views.begin(), pgShared_->views.end(), v) == pgShared_->views.end()) {
            // (its blocks' pages went into every hart's code pages: nothing
            // this hart has to drop yet)
            pgShared_->views.push_back(v);
            v->pgGen = h.codeGen;
        }
        return pgShared_.get();
    }
    // Bring the hart's translators up to date with what FENCE.I found.
    // pg: from the paged run loop, where v is a view of the shared one (only
    // a view's pgGen says what the shared one was synced to)
    void jitSync(Hart& h, Jit& v, bool pg = false) {
        Jit* s = serial && harts.size() > 1 ? pgShared_.get() : nullptr;
        if (v.gen != h.codeGen) { v.flush(); v.gen = h.codeGen; }
        if (s && pg && v.pgGen != h.codeGen) { s->flush(); v.pgGen = h.codeGen; }
        if (!h.codeChanged.empty()) {
            v.dropPages(h.codeChanged);
            if (s) {
                std::vector<uint64_t> gone;
                for (uint64_t pg : h.codeChanged) {
                    auto it = s->pgSnap.find(pg);
                    if (it != s->pgSnap.end() && !it->second.stale) {
                        Device* d = bus.find(pg << 12);
                        const uint8_t* hp = d && d->memory() ? d->memory() + ((pg << 12) - d->base) : nullptr;
                        if (hp && !memcmp(it->second.b.get(), hp, 4096)) {
                            // translated from what is there now: kept, so this
                            // hart's FENCE.I has to watch the page again (all of it)
                            noteCode(h, pg << 12, hp[0] | (uint32_t)hp[1] << 8, 2);
                            auto c = h.codePages.find(pg);
                            if (c != h.codePages.end()) c->second.used = ~0ull;
                            continue;
                        }
                    }
                    gone.push_back(pg);
                }
                s->dropPages(gone);
            }
            h.codeChanged.clear();
        }
    }
    // Translate the block at pc0 (in the hart's cached region). Null when
    // its first instruction cannot be fetched from there.
    // step: the version for the end of the budget, which counts each
    // instruction and leaves when the budget runs out.
    // pg: supervisor or user code with paging on, at physical address pa0;
    // the block stays in pc0's page.
    uint8_t* jitCompile(Jit& J, Hart& h, uint64_t pc0, bool step = false, bool pg = false, uint64_t pa0 = 0) {
        const uint64_t cb = h.cBase, region = h.cEnd > cb ? h.cEnd - cb : 0;
        const uint8_t* const cp = h.cPtr;
        const uint8_t* hp = nullptr;  // pg: the page in host memory
        if (pg) {
            const uint64_t pp = pa0 & ~0xfffull;
            Device* d = bus.find(pp);
            if (!d || !d->memory() || pp + 4096 > d->base + d->size) return nullptr;
            hp = d->memory() + (pp - d->base);
        }
        struct Op { uint64_t pc; uint8_t kind, rd, rs1, rs2, len; int32_t imm; };
        Op ops[JIT_OPS];
        unsigned n = 0;
        uint64_t p = pc0;
        bool ctl = false, toInterp = false;
        while (n < JIT_OPS) {
            const uint8_t* src;
            uint64_t avail;
            if (pg) {
                if ((p ^ pc0) >> 12) break;  // the next page: looked up
                src = hp + (p & 0xfff);
                avail = 4096 - (p & 0xfff);
            } else {
                const uint64_t off = p - cb;
                if (off >= region || region - off < 2) { toInterp = true; break; }
                src = cp + off;
                avail = region - off;
            }
            uint32_t raw = 0, insn;
            memcpy(&raw, src, avail >= 4 ? 4 : 2);
            unsigned len;
            if ((raw & 3) == 3) {
                if (avail < 4) { toInterp = true; break; }
                insn = raw; len = 4;
            } else {
                insn = rvc::expand(raw & 0xffff, true); len = 2;
                if (!insn) { toInterp = true; break; }
            }
            int32_t imm;
            uint8_t k = fastKind(insn, true, imm);
            if (!k) {
                k = jitKind(insn, pg);
                if (k == J_AMO && harts.size() > 1 && !serial) k = K_NONE;  // AMOs break other harts' reservations
                if (k == J_AMO) imm = (int32_t)((insn >> 27) | ((insn >> 12) & 7) << 8);  // funct5, width
                else if (k == J_CBOZ) imm = 0;
                else if (k == J_ZU || k == J_ZB) imm = zbbOp(insn);
                // CSRRCI on sstatus in supervisor mode: clears SIE at most
                // (bits 0 to 4), which never lets an interrupt in; and a
                // plain read (CSRRS or CSRRC with x0, or CSRRSI with 0)
                if (!k && pg && h.priv == 1 && (insn >> 20) == 0x100) {
                    if ((insn & 0x707f) == 0x7073) {
                        k = J_SSC;
                        imm = (int32_t)((insn >> 15) & 31);
                    } else if ((insn & 0xf807f) == 0x2073 || (insn & 0xf807f) == 0x3073 || (insn & 0xf807f) == 0x6073) {
                        k = J_SSC;
                        imm = 0;
                    }
                }
            }
            if (!k) { toInterp = true; break; }
            // (a shared block: any hart may run it, so FENCE.I on any has to look at its page)
            if (pg && &J == pgShared_.get())
                for (auto& o : harts) noteInsn(*o, (pa0 & ~0xfffull) | (p & 0xfff), len == 4 ? raw : raw & 0xffff, len);
            else noteInsn(h, pg ? (pa0 & ~0xfffull) | (p & 0xfff) : p, len == 4 ? raw : raw & 0xffff, len);
            ops[n++] = Op{p, k, (uint8_t)((insn >> 7) & 31), (uint8_t)((insn >> 15) & 31), (uint8_t)((insn >> 20) & 31),
                          (uint8_t)len, imm};
            p += len;
            if (k >= K_JAL && k <= K_BGEU) { ctl = true; break; }
        }
        if (!pg && !n && p - cb >= region) return nullptr;
        if (J.used + 16384 > Jit::BUF) J.flush();
        if (pg && n && &J == pgShared_.get()) {  // the bytes it was translated from (Jit::pgSnap)
            Jit::Snap& sn = J.pgSnap[pa0 >> 12];
            if (!sn.b) {
                sn.b.reset(new uint8_t[4096]);
                memcpy(sn.b.get(), hp, 4096);
            } else if (memcmp(sn.b.get() + (pc0 & 0xfff), hp + (pc0 & 0xfff), p - pc0)) sn.stale = true;
        }
        using R = Jit;
        J.p = J.buf + J.used;
        uint8_t* const entry = J.p;
        auto toEpi = [&] { Jit::fix(J.jmp(), J.epi); };
        auto leave = [&](uint64_t pc, int32_t why = 1) {  // to the fast loop at pc (2: to the step version)
            J.movI(R::RAX, pc);
            J.movCtxI(72, why);
            J.aluRR(R::A_XOR, R::RDX, R::RDX, false);
            toEpi();
        };
        auto keep = [&] {
            J.used = (size_t)(J.p - J.buf);
            if (pg) {
                Jit::PgBlock& e = J.pg[Jit::PgKey{pc0, pa0}];
                if (e.pa != pa0 || (!e.code && !e.step)) {
                    e = {pa0, nullptr, nullptr};
                    J.pgByPage[pa0 >> 12].push_back(pc0);
                }
                (step ? e.step : e.code) = entry;
                if (!step) J.compiled++;
            } else {
                for (uint64_t a = pc0 >> 12; a <= (p > pc0 ? (p - 1) >> 12 : pc0 >> 12); a++) J.barePages.insert(a);
                if (step) J.steps[pc0] = entry;
                else J.add(pc0, entry);
            }
            return entry;
        };
        if (!n) {  // the fast loop takes it from here
            leave(pc0);
            return keep();
        }
        // Which registers each op reads (rs1, rs2) and writes (rd).
        auto uses = [](uint8_t k, bool& r1, bool& r2, bool& wd) {
            r1 = k != K_LUI && k != K_AUIPC && k != K_JAL;
            r2 = (k >= K_BEQ && k <= K_BGEU) || (k >= K_SB && k <= K_SD) || (k >= K_ADD && k <= K_MUL) ||
                 (k >= K_ADDW && k <= K_MULW) || (k >= J_MULH && k <= J_REMUW) || k == J_AMO || k == J_ZB;
            wd = !(k >= K_BEQ && k <= K_BGEU) && !(k >= K_SB && k <= K_SD) && k != J_FENCE && k != J_CBOZ;
            if (k == J_SSC) r1 = r2 = false;
        };
        // The block's busiest guest registers live in host registers from its
        // start (those it reads before writing are loaded) to each exit
        // (those it wrote are stored back).
        // (with paging, r11 is the page cache entry's address)
        static const int poolBare[] = {R::RSI, R::RDI, R::R8, R::R9, R::R10, R::R11, R::RDX};
        static const int poolPg[] = {R::RSI, R::RDI, R::R8, R::R9, R::R10, R::RDX};
        bool amo = false;  // (AMOs, LR and SC use r11 too)
        for (unsigned k = 0; k < n; k++) amo |= ops[k].kind == J_AMO;
        const int* const pool = pg || amo ? poolPg : poolBare;
        const unsigned npool = pg || amo ? 6 : 7;
        int hr[33];
        for (int& x : hr) x = -1;
        uint32_t liveIn = 0;
        {
            unsigned cnt[32] = {};
            uint32_t written = 0;
            for (unsigned k = 0; k < n; k++) {
                bool r1, r2, wd;
                uses(ops[k].kind, r1, r2, wd);
                auto rd_ = [&](unsigned g) { if (!g) return; cnt[g]++; if (!(written >> g & 1)) liveIn |= 1u << g; };
                if (r1) rd_(ops[k].rs1);
                if (r2) rd_(ops[k].rs2);
                if (wd && ops[k].rd) { cnt[ops[k].rd]++; written |= 1u << ops[k].rd; }
            }
            bool rdxUsed = false;  // MULH* and the divisions need rdx
            for (unsigned k = 0; k < n; k++) rdxUsed |= ops[k].kind >= J_MULH && ops[k].kind <= J_REMUW;
            for (unsigned i = 0; i < npool - rdxUsed; i++) {
                unsigned best = 0;
                for (unsigned g = 1; g < 32; g++)
                    if (hr[g] < 0 && cnt[g] > cnt[best]) best = g;
                if (!best || cnt[best] < 2) break;
                hr[best] = pool[i];
            }
        }
        uint32_t dirty = 0;
        // why: 0 run the interpreter from the op; else a page cache miss of a
        // load or store (3 | store << 8 | bytes << 16) that the run loop may
        // fill and come back for (Machine::jitRunPg)
        struct Bail { uint8_t* rel; unsigned k; uint32_t dirty; uint32_t why = 0; };
        struct Link { uint8_t* rel; uint64_t to; };
        Bail bails[JIT_OPS * 3], stops[JIT_OPS];
        uint8_t* opAt[JIT_OPS];  // where each op's code starts
        Link links[2];
        unsigned nb = 0, nl = 0, ns = 0;
        uint8_t* budget = nullptr;
        if (!step) {
            J.aluI(R::A_CMP, R::R12, (int32_t)n);
            budget = J.jcc(R::CC_B);
            J.aluI(R::A_SUB, R::R12, (int32_t)n);
        }
        for (unsigned g = 1; g < 32; g++)
            if (hr[g] >= 0 && (liveIn >> g & 1)) J.ldg(hr[g], g);
        auto writeback = [&](uint32_t m) {
            for (unsigned g = 1; g < 32; g++)
                if (m >> g & 1) J.stg(g, hr[g]);
        };
        // Operands: reg = x[g]; reg op= x[g]; x[rd] = reg.
        auto ld = [&](int reg, unsigned g, bool w = true) {
            if (!g) J.aluRR(R::A_XOR, reg, reg, false);
            else if (hr[g] >= 0) J.movRR(reg, hr[g], w);
            else J.ldg(reg, g, w);
        };
        auto with = [&](Jit::Alu a, int reg, unsigned g, bool w = true) {
            if (!g) J.aluI(a, reg, 0, w);
            else if (hr[g] >= 0) J.aluRR(a, reg, hr[g], w);
            else J.aluG(a, reg, g, w);
        };
        auto mulWith = [&](int reg, unsigned g, bool w) {
            if (!g) J.aluRR(R::A_XOR, reg, reg, false);
            else if (hr[g] >= 0) { J.rex(w, reg, hr[g]); J.b(0x0f); J.b(0xaf); J.modrr(reg, hr[g]); }
            else J.imulG(reg, g, w);
        };
        auto put = [&](unsigned rd, int reg = R::RAX) {
            if (!rd) return;
            if (hr[rd] >= 0) { J.movRR(hr[rd], reg); dirty |= 1u << rd; }
            else J.stg(rd, reg);
        };
        auto chain = [&](uint64_t to) { links[nl++] = Link{J.jmp(), to}; };
        // Go to the block at rax through the indirect-jump table, or leave
        // for the run loop to find it.
        auto jtLook = [&] {
            J.movRR(R::RCX, R::RAX, false);
            J.shiftI(5, R::RCX, 1, false);
            J.aluI(R::A_AND, R::RCX, (int32_t)(Jit::JT - 1), false);
            J.shiftI(4, R::RCX, 4);
            J.aluC(R::A_ADD, R::RCX, 80);
            J.rex(true, R::RAX, R::RCX); J.b(0x3b); J.mem(R::RAX, R::RCX, 0);  // cmp rax, [rcx]
            uint8_t* miss = J.jcc(R::CC_NE);
            J.rex(false, 4, R::RCX); J.b(0xff); J.mem(4, R::RCX, 8);           // jmp [rcx + 8]
            Jit::fix(miss, J.p);
            J.aluRR(R::A_XOR, R::RDX, R::RDX, false);
            toEpi();
        };
        for (unsigned k = 0; k < n; k++) {
            const Op& o = ops[k];
            const int32_t imm = o.imm;
            const uint64_t next = o.pc + o.len;
            auto addr = [&] {  // rax = rs1 + imm (an AMO's imm is its funct5 and width)
                ld(R::RAX, o.rs1);
                if (imm && o.kind != J_AMO) J.aluI(R::A_ADD, R::RAX, imm);
            };
            auto shift = [&](int ext, bool w) {
                ld(R::RCX, o.rs2, w);
                ld(R::RAX, o.rs1, w);
                J.shiftCl(ext, R::RAX, w);
                if (!w) J.movsxd(R::RAX, R::RAX);
                put(o.rd);
            };
            auto set = [&](Jit::Cc c, bool reg) {
                J.aluRR(R::A_XOR, R::RCX, R::RCX, false);
                ld(R::RAX, o.rs1);
                if (reg) with(R::A_CMP, R::RAX, o.rs2); else J.aluI(R::A_CMP, R::RAX, imm);
                J.setcc(c, R::RCX);
                put(o.rd, R::RCX);
            };
            auto alu = [&](Jit::Alu a, bool w) {
                ld(R::RAX, o.rs1, w);
                with(a, R::RAX, o.rs2, w);
                if (!w) J.movsxd(R::RAX, R::RAX);
                put(o.rd);
            };
            auto alui = [&](Jit::Alu a) {
                ld(R::RAX, o.rs1);
                J.aluI(a, R::RAX, imm);
                put(o.rd);
            };
            auto shifti = [&](int ext, bool w) {
                ld(R::RAX, o.rs1, w);
                if (imm) J.shiftI(ext, R::RAX, (unsigned)imm, w);
                if (!w) J.movsxd(R::RAX, R::RAX);
                put(o.rd);
            };
            auto bail = [&](Jit::Cc c, uint32_t why = 0) { bails[nb++] = Bail{J.jcc(c), k, dirty, why}; };
            // Loads and stores: ad - cb in rax, checked against the span.
            // With paging: the entry for ad's page in the hart's page cache
            // (r11), checked; rax = where ad is in host memory.
            auto pageOf = [&](int32_t cache, unsigned nbytes, bool aligned = false) {
                addr();
                if (o.kind == J_CBOZ) J.aluI(R::A_AND, R::RAX, -64);
                if (aligned) {  // else the general path raises the exception
                    J.rex(false, 0, R::RAX); J.b(0xa9); J.d32(nbytes - 1);  // test eax, n - 1
                    bail(R::CC_NE);
                }
                J.movRR(R::RCX, R::RAX);                                    // the last byte's tag
                if (nbytes > 1) J.aluI(R::A_ADD, R::RCX, (int32_t)nbytes - 1);
                J.shiftI(5, R::RCX, 12);
                J.aluC(R::A_OR, R::RCX, 104);
                J.movRR(R::R11, R::RAX);                                    // the first byte's entry
                J.shiftI(5, R::R11, 12);
                J.aluI(R::A_AND, R::R11, (int32_t)(Hart::DP_SIZE - 1), false);
                J.shiftI(4, R::R11, 4);
                J.aluC(R::A_ADD, R::R11, cache);
                J.rex(true, R::RCX, R::R11); J.b(0x3b); J.mem(R::RCX, R::R11, 0);  // cmp rcx, [r11].tag
                const bool ls = !step && o.kind >= K_LB && o.kind <= K_SD;
                bail(R::CC_NE, ls ? 3u | (cache == 96 ? 1u : 0u) << 8 | nbytes << 16 : 0u);
                J.aluI(R::A_AND, R::RAX, 0xfff, false);
                J.rex(true, R::RAX, R::R11); J.b(0x03); J.mem(R::RAX, R::R11, (int32_t)offsetof(Hart::DataPage, host));
            };
            auto load = [&](std::initializer_list<uint8_t> opc, bool w, unsigned nbytes) {
                if (pg) {
                    pageOf(88, nbytes);
                    J.rex(w, R::RAX, R::RAX);
                    for (uint8_t c : opc) J.b(c);
                    J.mem(R::RAX, R::RAX, 0);
                    put(o.rd);
                    return;
                }
                addr();
                J.aluRR(R::A_SUB, R::RAX, R::R14);
                J.aluRR(R::A_CMP, R::RAX, R::R15);
                bail(R::CC_AE);
                J.rex(w, R::RAX, R::R13, R::RAX);
                for (uint8_t c : opc) J.b(c);
                J.memIdx(R::RAX, R::R13, R::RAX);
                put(o.rd);
            };
            auto store = [&](unsigned nbytes) {
                if (pg) {
                    pageOf(96, nbytes);
                    ld(R::RCX, o.rs2);
                    if (nbytes == 2) J.b(0x66);
                    J.rex(nbytes == 8, R::RCX, R::RAX);
                    J.b(nbytes == 1 ? 0x88 : 0x89);
                    J.mem(R::RCX, R::RAX, 0);
                    return;
                }
                addr();
                J.movRR(R::RCX, R::RAX);                  // tohost: ad + n - 1 - th < 7 + n leaves
                J.aluC(R::A_SUB, R::RCX, 64);
                if (nbytes > 1) J.aluI(R::A_ADD, R::RCX, (int32_t)nbytes - 1);
                J.aluI(R::A_CMP, R::RCX, (int32_t)(7 + nbytes));
                bail(R::CC_B);
                J.aluRR(R::A_SUB, R::RAX, R::R14);
                J.aluC(R::A_CMP, R::RAX, 56);
                bail(R::CC_AE);
                ld(R::RCX, o.rs2);
                if (nbytes == 2) J.b(0x66);
                J.rex(nbytes == 8, R::RCX, R::R13, R::RAX);
                J.b(nbytes == 1 ? 0x88 : 0x89);
                J.memIdx(R::RCX, R::R13, R::RAX);
            };
            // An AMO's, LR's or SC's word in host memory, in rax (aligned,
            // else the general path raises the exception).
            auto amoPtr = [&](bool st, unsigned nbytes) {
                if (pg) { pageOf(st ? 96 : 88, nbytes, true); return; }
                addr();
                J.rex(false, 0, R::RAX); J.b(0xa9); J.d32(nbytes - 1);  // test eax, n - 1
                bail(R::CC_NE);
                if (st) {  // not within 8 bytes of tohost
                    J.movRR(R::RCX, R::RAX);
                    J.aluC(R::A_SUB, R::RCX, 64);
                    J.aluI(R::A_ADD, R::RCX, (int32_t)nbytes - 1);
                    J.aluI(R::A_CMP, R::RCX, (int32_t)(7 + nbytes));
                    bail(R::CC_B);
                }
                J.aluRR(R::A_SUB, R::RAX, R::R14);
                if (st) J.aluC(R::A_CMP, R::RAX, 56); else J.aluRR(R::A_CMP, R::RAX, R::R15);
                bail(R::CC_AE);
                J.aluRR(R::A_ADD, R::RAX, R::R13);
            };
            const bool term = o.kind >= K_JAL && o.kind <= K_BGEU;
            opAt[k] = J.p;
            if (step && term) J.aluI(R::A_SUB, R::R12, 1);
            switch (o.kind) {
                case K_LUI: J.movI(R::RAX, (uint64_t)(int64_t)imm); put(o.rd); break;
                case K_AUIPC: J.movI(R::RAX, o.pc + (uint64_t)(int64_t)imm); put(o.rd); break;
                case K_JAL:
                    if (o.rd) { J.movI(R::RAX, next); put(o.rd); }
                    writeback(dirty);
                    chain(o.pc + (uint64_t)(int64_t)imm);
                    break;
                case K_JALR: {
                    addr();
                    J.aluI(R::A_AND, R::RAX, -2);
                    if (o.rd) { J.movI(R::RCX, next); put(o.rd, R::RCX); }
                    writeback(dirty);
                    jtLook();
                    break;
                }
                case K_BEQ: case K_BNE: case K_BLT: case K_BGE: case K_BLTU: case K_BGEU: {
                    static const Jit::Cc cc[6] = {R::CC_E, R::CC_NE, R::CC_L, R::CC_GE, R::CC_B, R::CC_AE};
                    writeback(dirty);
                    ld(R::RAX, o.rs1);
                    with(R::A_CMP, R::RAX, o.rs2);
                    links[nl++] = Link{J.jcc(cc[o.kind - K_BEQ]), o.pc + (uint64_t)(int64_t)imm};
                    chain(next);
                    break;
                }
                case K_LB: load({0x0f, 0xbe}, true, 1); break;
                case K_LH: load({0x0f, 0xbf}, true, 2); break;
                case K_LW: load({0x63}, true, 4); break;
                case K_LD: load({0x8b}, true, 8); break;
                case K_LBU: load({0x0f, 0xb6}, false, 1); break;
                case K_LHU: load({0x0f, 0xb7}, false, 2); break;
                case K_LWU: load({0x8b}, false, 4); break;
                case K_SB: store(1); break;
                case K_SH: store(2); break;
                case K_SW: store(4); break;
                case K_SD: store(8); break;
                case K_ADDI:
                    if (!o.rs1) J.movI(R::RAX, (uint64_t)(int64_t)imm);
                    else { ld(R::RAX, o.rs1); if (imm) J.aluI(R::A_ADD, R::RAX, imm); }
                    put(o.rd);
                    break;
                case K_SLLI: shifti(4, true); break;
                case K_SRLI: shifti(5, true); break;
                case K_SRAI: shifti(7, true); break;
                case K_SLTI: set(R::CC_L, false); break;
                case K_SLTIU: set(R::CC_B, false); break;
                case K_XORI: alui(R::A_XOR); break;
                case K_ORI: alui(R::A_OR); break;
                case K_ANDI: alui(R::A_AND); break;
                case K_ADD: alu(R::A_ADD, true); break;
                case K_SUB: alu(R::A_SUB, true); break;
                case K_XOR: alu(R::A_XOR, true); break;
                case K_OR: alu(R::A_OR, true); break;
                case K_AND: alu(R::A_AND, true); break;
                case K_SLL: shift(4, true); break;
                case K_SRL: shift(5, true); break;
                case K_SRA: shift(7, true); break;
                case K_SLT: set(R::CC_L, true); break;
                case K_SLTU: set(R::CC_B, true); break;
                case K_MUL: ld(R::RAX, o.rs1); mulWith(R::RAX, o.rs2, true); put(o.rd); break;
                case K_ADDIW:
                    ld(R::RAX, o.rs1, false);
                    if (imm) J.aluI(R::A_ADD, R::RAX, imm, false);
                    J.movsxd(R::RAX, R::RAX);
                    put(o.rd);
                    break;
                case K_SLLIW: shifti(4, false); break;
                case K_SRLIW: shifti(5, false); break;
                case K_SRAIW: shifti(7, false); break;
                case K_ADDW: alu(R::A_ADD, false); break;
                case K_SUBW: alu(R::A_SUB, false); break;
                case K_SLLW: shift(4, false); break;
                case K_SRLW: shift(5, false); break;
                case K_SRAW: shift(7, false); break;
                case K_MULW:
                    ld(R::RAX, o.rs1, false);
                    mulWith(R::RAX, o.rs2, false);
                    J.movsxd(R::RAX, R::RAX);
                    put(o.rd);
                    break;
                case J_MULH: case J_MULHU: case J_MULHSU:
                    ld(R::RAX, o.rs1);
                    ld(R::RCX, o.rs2);
                    J.rex(true, 0, R::RCX); J.b(0xf7); J.modrr(o.kind == J_MULH ? 5 : 4, R::RCX);  // (i)mul rcx
                    if (o.kind == J_MULHSU) {  // signed rs1: take rs2 off the unsigned high half when rs1 < 0
                        ld(R::RAX, o.rs1);
                        J.shiftI(7, R::RAX, 63);
                        J.aluRR(R::A_AND, R::RAX, R::RCX);
                        J.aluRR(R::A_SUB, R::RDX, R::RAX);
                    }
                    put(o.rd, R::RDX);
                    break;
                case J_DIV: case J_DIVU: case J_REM: case J_REMU: case J_DIVW: case J_DIVUW: case J_REMW: case J_REMUW: {
                    const bool w = o.kind <= J_REMU;
                    const unsigned v = (o.kind - J_DIV) & 3;  // 0 div, 1 divu, 2 rem, 3 remu
                    const bool sgn = !(v & 1), rem = v >= 2;
                    ld(R::RCX, o.rs2, w);
                    ld(R::RAX, o.rs1, w);
                    J.rex(w, R::RCX, R::RCX); J.b(0x85); J.modrr(R::RCX, R::RCX);  // test rcx, rcx
                    uint8_t* zero = J.jcc(R::CC_E);
                    uint8_t* done1 = nullptr;
                    if (sgn) {  // by -1: the negation (wrapping) or 0, which idiv would trap on
                        J.aluI(R::A_CMP, R::RCX, -1, w);
                        uint8_t* go = J.jcc(R::CC_NE);
                        if (rem) J.aluRR(R::A_XOR, R::RAX, R::RAX, false);
                        else { J.rex(w, 0, R::RAX); J.b(0xf7); J.modrr(3, R::RAX); }  // neg
                        done1 = J.jmp();
                        Jit::fix(go, J.p);
                        if (w) J.b(0x48);
                        J.b(0x99);  // cqo / cdq
                    } else J.aluRR(R::A_XOR, R::RDX, R::RDX, false);
                    J.rex(w, 0, R::RCX); J.b(0xf7); J.modrr(sgn ? 7 : 6, R::RCX);  // (i)div rcx
                    if (rem) J.movRR(R::RAX, R::RDX);
                    uint8_t* done2 = J.jmp();
                    Jit::fix(zero, J.p);  // by zero: all ones, or the dividend
                    if (!rem) J.movI(R::RAX, ~0ull);
                    Jit::fix(done2, J.p);
                    if (done1) Jit::fix(done1, J.p);
                    if (!w) J.movsxd(R::RAX, R::RAX);
                    put(o.rd);
                    break;
                }
                case J_AMO: {  // rd = old value; memory = op(old, rs2)
                    const unsigned f5 = (unsigned)imm & 0xff;
                    const bool w = ((unsigned)imm >> 8) == 3;
                    if (f5 == 0x02) {  // LR: the reservation, then the load
                        addr();
                        J.ldCtx(R::R11, 120);
                        J.rex(true, R::RAX, R::R11); J.b(0x89); J.mem(R::RAX, R::R11, 0);    // resvAddr = a
                        J.ldCtx(R::R11, 112);
                        J.rex(false, 0, R::R11); J.b(0xc6); J.mem(0, R::R11, 0); J.b(1);    // resv = 1
                        amoPtr(false, w ? 8 : 4);
                        J.rex(w, R::RCX, R::RAX); J.b(0x8b); J.mem(R::RCX, R::RAX, 0);
                        if (!w) J.movsxd(R::RCX, R::RCX);
                        put(o.rd, R::RCX);
                        break;
                    }
                    if (f5 == 0x03) {  // SC: stores when the reservation is for a
                        addr();
                        J.ldCtx(R::R11, 120);
                        J.rex(true, R::RAX, R::R11); J.b(0x3b); J.mem(R::RAX, R::R11, 0);    // cmp rax, resvAddr
                        uint8_t* f1 = J.jcc(R::CC_NE);
                        J.ldCtx(R::R11, 112);
                        J.rex(false, 7, R::R11); J.b(0x80); J.mem(7, R::R11, 0); J.b(0);    // cmp byte resv, 0
                        uint8_t* f2 = J.jcc(R::CC_E);
                        amoPtr(true, w ? 8 : 4);
                        ld(R::RCX, o.rs2, w);
                        J.rex(w, R::RCX, R::RAX); J.b(0x89); J.mem(R::RCX, R::RAX, 0);
                        J.aluRR(R::A_XOR, R::RAX, R::RAX, false);
                        uint8_t* done = J.jmp();
                        Jit::fix(f1, J.p);
                        Jit::fix(f2, J.p);
                        J.movI(R::RAX, 1);
                        Jit::fix(done, J.p);
                        J.ldCtx(R::R11, 112);
                        J.rex(false, 0, R::R11); J.b(0xc6); J.mem(0, R::R11, 0); J.b(0);    // resv = 0
                        put(o.rd);
                        break;
                    }
                    amoPtr(true, w ? 8 : 4);
                    ld(R::RCX, o.rs2, w);
                    int res = R::RCX;
                    if (f5 == 0x01) {        // xchg [rax], rcx
                        J.rex(w, R::RCX, R::RAX); J.b(0x87); J.mem(R::RCX, R::RAX, 0);
                    } else if (f5 == 0x00) { // xadd [rax], rcx
                        J.rex(w, R::RCX, R::RAX); J.b(0x0f); J.b(0xc1); J.mem(R::RCX, R::RAX, 0);
                    } else {
                        J.rex(w, R::R11, R::RAX); J.b(0x8b); J.mem(R::R11, R::RAX, 0);  // r11 = old
                        static const uint8_t cmov[4] = {0x4c, 0x4f, 0x42, 0x47};  // min: l, max: g, minu: b, maxu: a
                        if (f5 == 0x04) J.aluRR(R::A_XOR, R::RCX, R::R11, w);
                        else if (f5 == 0x08) J.aluRR(R::A_OR, R::RCX, R::R11, w);
                        else if (f5 == 0x0c) J.aluRR(R::A_AND, R::RCX, R::R11, w);
                        else {  // rcx = rs2; if old (cc) rs2, rcx = old
                            J.aluRR(R::A_CMP, R::R11, R::RCX, w);
                            J.rex(w, R::RCX, R::R11); J.b(0x0f); J.b(cmov[(f5 - 0x10) >> 2]); J.modrr(R::RCX, R::R11);
                        }
                        J.rex(w, R::RCX, R::RAX); J.b(0x89); J.mem(R::RCX, R::RAX, 0);  // store
                        res = R::R11;
                    }
                    if (!w) J.movsxd(res, res);
                    put(o.rd, res);
                    break;
                }
                case J_SSC: {  // rd = sstatus; mstatus &= ~(uimm & SSTATUS_MASK)
                    J.ldCtx(R::R11, 128);
                    J.rex(true, R::RAX, R::R11); J.b(0x8b); J.mem(R::RAX, R::R11, 0);       // rax = mstatus
                    J.movI(R::RCX, SSTATUS_MASK);
                    J.aluRR(R::A_AND, R::RCX, R::RAX);
                    // SD: FS or VS dirty
                    J.movRR(R::R11, R::RAX);
                    J.aluI(R::A_AND, R::R11, (int32_t)MSTATUS_FS);
                    J.aluI(R::A_CMP, R::R11, (int32_t)MSTATUS_FS);
                    uint8_t* sd1 = J.jcc(R::CC_E);
                    J.movRR(R::R11, R::RAX);
                    J.aluI(R::A_AND, R::R11, (int32_t)MSTATUS_VS);
                    J.aluI(R::A_CMP, R::R11, (int32_t)MSTATUS_VS);
                    uint8_t* nosd = J.jcc(R::CC_NE);
                    Jit::fix(sd1, J.p);
                    J.rex(true, 0, R::RCX); J.b(0x0f); J.b(0xba); J.modrr(5, R::RCX); J.b(63);  // bts rcx, 63
                    Jit::fix(nosd, J.p);
                    J.movI(R::R11, 2ull << 32);  // UXL: 64
                    J.aluRR(R::A_OR, R::RCX, R::R11);
                    if (const uint32_t m = (uint32_t)imm & SSTATUS_MASK) {
                        J.aluI(R::A_AND, R::RAX, (int32_t)~m);
                        J.ldCtx(R::R11, 128);
                        J.rex(true, R::RAX, R::R11); J.b(0x89); J.mem(R::RAX, R::R11, 0);
                    }
                    put(o.rd, R::RCX);
                    break;
                }
                case J_ZU: case J_ZB: {
                    const unsigned z = (unsigned)imm & 0xff;
                    const bool w = !(z == Z_CLZW || z == Z_CTZW || z == Z_CPOPW || z == Z_RORIW || z == Z_ROLW || z == Z_RORW);
                    auto cmov = [&](uint8_t cc, int dst, int src) { J.rex(true, dst, src); J.b(0x0f); J.b((uint8_t)(0x40 + cc)); J.modrr(dst, src); };
                    if (o.kind == J_ZB) ld(R::RCX, o.rs2);
                    ld(R::RAX, o.rs1, w);
                    switch (z) {
                        case Z_CLZ: case Z_CLZW:  // (w - 1) - bsr, -1 for zero
                            J.movI(R::RCX, ~0ull);
                            J.rex(w, R::RAX, R::RAX); J.b(0x0f); J.b(0xbd); J.modrr(R::RAX, R::RAX);
                            cmov(0x4, R::RAX, R::RCX);
                            J.rex(true, 0, R::RAX); J.b(0xf7); J.modrr(3, R::RAX);  // neg
                            J.aluI(R::A_ADD, R::RAX, w ? 63 : 31);
                            break;
                        case Z_CTZ: case Z_CTZW:  // bsf, w for zero
                            J.movI(R::RCX, w ? 64 : 32);
                            J.rex(w, R::RAX, R::RAX); J.b(0x0f); J.b(0xbc); J.modrr(R::RAX, R::RAX);
                            cmov(0x4, R::RAX, R::RCX);
                            break;
                        case Z_CPOP: case Z_CPOPW:
                            J.b(0xf3); J.rex(w, R::RAX, R::RAX); J.b(0x0f); J.b(0xb8); J.modrr(R::RAX, R::RAX);
                            break;
                        case Z_SEXTB: J.rex(true, R::RAX, R::RAX); J.b(0x0f); J.b(0xbe); J.modrr(R::RAX, R::RAX); break;
                        case Z_SEXTH: J.rex(true, R::RAX, R::RAX); J.b(0x0f); J.b(0xbf); J.modrr(R::RAX, R::RAX); break;
                        case Z_ZEXTH: J.b(0x0f); J.b(0xb7); J.modrr(R::RAX, R::RAX); break;
                        case Z_ORCB:  // bit 7 of ((x & 0x7f..) + 0x7f..) | x: the byte is not zero
                            J.movI(R::RCX, 0x7f7f7f7f7f7f7f7full);
                            J.aluRR(R::A_AND, R::RAX, R::RCX);
                            J.aluRR(R::A_ADD, R::RAX, R::RCX);
                            with(R::A_OR, R::RAX, o.rs1);
                            J.shiftI(5, R::RAX, 7);
                            J.movI(R::RCX, 0x0101010101010101ull);
                            J.aluRR(R::A_AND, R::RAX, R::RCX);
                            J.rex(true, R::RAX, R::RAX); J.b(0x69); J.modrr(R::RAX, R::RAX); J.d32(0xff);  // imul rax, rax, 255
                            break;
                        case Z_REV8: J.rex(true, 0, R::RAX); J.b(0x0f); J.b(0xc8); break;  // bswap rax
                        case Z_RORI: case Z_RORIW:
                            if (imm >> 8) J.shiftI(1, R::RAX, (unsigned)imm >> 8, w);
                            break;
                        case Z_ANDN: case Z_ORN:
                            J.rex(true, 0, R::RCX); J.b(0xf7); J.modrr(2, R::RCX);  // not rcx
                            J.aluRR(z == Z_ANDN ? R::A_AND : R::A_OR, R::RAX, R::RCX);
                            break;
                        case Z_XNOR:
                            J.aluRR(R::A_XOR, R::RAX, R::RCX);
                            J.rex(true, 0, R::RAX); J.b(0xf7); J.modrr(2, R::RAX);
                            break;
                        case Z_MIN: case Z_MINU: case Z_MAX: case Z_MAXU:
                            J.aluRR(R::A_CMP, R::RAX, R::RCX);
                            cmov(z == Z_MIN ? 0xf : z == Z_MINU ? 0x7 : z == Z_MAX ? 0xc : 0x2, R::RAX, R::RCX);
                            break;
                        case Z_ROL: case Z_ROLW: J.shiftCl(0, R::RAX, w); break;
                        case Z_ROR: case Z_RORW: J.shiftCl(1, R::RAX, w); break;
                        default: break;
                    }
                    if (!w) J.movsxd(R::RAX, R::RAX);
                    put(o.rd);
                    break;
                }
                case J_CBOZ:  // 64 zero bytes at rs1 & ~63
                    pageOf(96, 64);
                    J.aluRR(R::A_XOR, R::RCX, R::RCX, false);
                    for (int32_t i = 0; i < 64; i += 8) { J.rex(true, R::RCX, R::RAX); J.b(0x89); J.mem(R::RCX, R::RAX, i); }
                    break;
                default: break;
            }
            if (step && !term) {
                J.aluI(R::A_SUB, R::R12, 1);
                stops[ns++] = Bail{J.jcc(R::CC_E), k + 1, dirty};
            }
        }
        if (!ctl) {
            writeback(dirty);
            if (toInterp) leave(p);
            else chain(p);
        }
        // Out of line: the exits.
        if (budget) {
            Jit::fix(budget, J.p);
            leave(pc0, 2);
        }
        for (unsigned i = 0; i < nb; i++) {
            const Bail& bl = bails[i];
            const int32_t rest = (int32_t)(n - bl.k);  // the ops from this one on
            uint8_t* resume = nullptr;
            if (bl.why) {
                // Back in after a fill: the budget for the rest of the block,
                // every register this block keeps in a host register (memory
                // has them all after the write-back below), the op again.
                resume = J.p;
                J.aluI(R::A_CMP, R::R12, rest);
                uint8_t* shortb = J.jcc(R::CC_B);
                J.aluI(R::A_SUB, R::R12, rest);
                for (unsigned g = 1; g < 32; g++)
                    if (hr[g] >= 0) J.ldg(hr[g], g);
                Jit::fix(J.jmp(), opAt[bl.k]);
                Jit::fix(shortb, J.p);
                leave(ops[bl.k].pc);
            }
            Jit::fix(bl.rel, J.p);
            if (bl.why) J.stCtx(136, R::RAX);  // the address
            writeback(bl.dirty);
            if (!step) J.aluI(R::A_ADD, R::R12, rest);
            if (bl.why) {
                J.movI(R::RCX, (uint64_t)(uintptr_t)resume);
                J.stCtx(144, R::RCX);
            }
            leave(ops[bl.k].pc, bl.why ? (int32_t)bl.why : 1);
        }
        for (unsigned i = 0; i < ns; i++) {  // the budget ran out
            Jit::fix(stops[i].rel, J.p);
            writeback(stops[i].dirty);
            leave(stops[i].k < n ? ops[stops[i].k].pc : p);
        }
        for (unsigned i = 0; i < nl; i++) {
            Jit::fix(links[i].rel, J.p);
            J.movI(R::RAX, links[i].to);
            if (pg && (links[i].to ^ pc0) >> 12) { jtLook(); continue; }  // another page: by its translation now
            J.movI(R::RDX, (uint64_t)(uintptr_t)links[i].rel);
            toEpi();
        }
        return keep();
    }
    // Without paging, where PMP is in force: may pc..pc+len be fetched? A
    // page found executable as a whole goes in the fetch page record (as
    // its own physical page; emptied with it, by PMP writes and satp), so
    // the next fetches from it need no PMP walk.
    bool bareExecOk(Hart& h, uint64_t pc, unsigned len) {
        Hart::FetchPage& e = h.fp[(pc >> 12) & (Hart::FP_SIZE - 1)];
        if (e.vpn == pc >> 12 && e.priv == h.priv && (pc & 0xfff) + len <= 4096) return true;
        if (!pmpAllows(h, pc, len, PMP_X, h.priv)) return false;
        const uint64_t pg = pc & ~0xfffull;
        if (pmpAllows(h, pg, 4096, PMP_X, h.priv)) e = {pc >> 12, h.priv, pg};
        return true;
    }
    // Does the decoded cache hold the instruction at pc for the fast loop?
    // (Else it would leave at once: the general path decodes it first.)
    bool turboReady(const Hart& h) const {
        uint64_t pc = h.pc, fx = 0;
        if (h.priv != 3 && h.paging()) {
            const Hart::FetchPage& e = h.fp[(pc >> 12) & (Hart::FP_SIZE - 1)];
            if (e.vpn != pc >> 12 || e.priv != h.priv) return false;
            pc = e.pa | (pc & 0xfff);
            fx = 1;
        }
        const Hart::Decoded& d = h.dc[(pc >> 1) & (Hart::DC_SIZE - 1)];
        return d.pc == ((pc | fx) ^ h.mmuTag) && d.kind;
    }
    // An instruction of n bytes (raw) was decoded from physical address fa:
    // record its page for FENCE.I (see Hart::codePages).
    void noteCode(Hart& h, uint64_t fa, uint32_t raw, unsigned n) {
        if (h.codeUntracked) return;
        const uint64_t pg = fa >> 12;
        auto it = h.codePages.find(pg);
        const uint64_t pieces = (1ull << ((fa & 0xfff) >> 6)) | (1ull << (((fa & 0xfff) + n - 1) >> 6));
        if (it != h.codePages.end()) {
            // decoded from bytes the copy does not hold: it changed already
            if (!it->second.changed && memcmp(it->second.snap.get() + (fa & 0xfff), &raw, n)) {
                it->second.changed = true;
                h.fenceQuiet = false;
            }
            it->second.used |= pieces;
            return;
        }
        Device* d = bus.find(pg << 12);
        if (h.codePages.size() >= Hart::CODE_PAGES || !d || !d->memory() || (pg << 12) + 4096 > d->base + d->size ||
            (h.par && h.ov.find(pg))) {
            h.codeUntracked = true;
            return;
        }
        Hart::CodePage cp{d->memory() + ((pg << 12) - d->base), std::unique_ptr<uint8_t[]>(new uint8_t[4096]), false, pieces, -1, 0};
        // watched first, then copied: a write after the copy counts
        if (!d->writable()) cp.wr = -2;
        else if (int id = watchRegion(d); id >= 0) {
            const size_t wp = ((pg << 12) - d->base) >> 12;
            if (WriteWatch::watch(id, wp)) { cp.wr = id; cp.seen = WriteWatch::gen(id, wp); }
        }
        memcpy(cp.snap.get(), cp.host, 4096);
        if (memcmp(cp.snap.get() + (fa & 0xfff), &raw, n)) cp.changed = true;
        if (cp.changed || cp.wr == -1) h.fenceQuiet = false;
        h.codePages.emplace(pg, std::move(cp));
    }
    void noteInsn(Hart& h, uint64_t fa, uint32_t raw, unsigned len) {
        if ((fa & 0xfff) + len > 4096) {  // across two pages
            noteCode(h, fa, raw, 2);
            noteCode(h, fa + 2, raw >> 16, 2);
        } else noteCode(h, fa, raw, len);
    }
    // The device's memory as a WriteWatch region (-1: not watched).
    // Harts of a parallel round call it at once (only for a page not seen
    // before, so a lock is cheap enough).
    int watchRegion(Device* d) {
        std::lock_guard<std::mutex> l(wrMu_);
        auto it = wrIds_.find(d);
        if (it != wrIds_.end()) return it->second;
        const int id = (d->base & 4095) || (d->size & 4095) ? -1 : WriteWatch::add(d->memory(), d->size);
        wrIds_[d] = id;
        return id;
    }
    std::mutex wrMu_;
    std::unordered_map<Device*, int> wrIds_;
    // FENCE.I: forget what was decoded from pages that changed.
    void fenceI(Hart& h) {
        if (h.codeUntracked || (h.par && !h.ov.empty())) {
            h.flushDecoded();
            h.fenceFull++;
            return;
        }
        h.fencePart++;
        // Every page watched and unchanged at the last FENCE.I, and nothing
        // written to a watched page since: nothing to look at.
        const uint64_t tw = WriteWatch::writes();
        if (h.fenceQuiet && tw == h.fenceWrites) return;
        bool any = false, quiet = true;
        for (auto it = h.codePages.begin(); it != h.codePages.end();) {
            Hart::CodePage& c = it->second;
            // not written since it was last compared (or read-only)
            if (!c.changed && c.wr != -1) {
                if (c.wr == -2) { ++it; continue; }
                const size_t wp = (size_t)(c.host - WriteWatch::base(c.wr)) >> 12;
                if (WriteWatch::readOnly(c.wr, wp) && WriteWatch::gen(c.wr, wp) == c.seen) { ++it; continue; }
            }
            // watched again before comparing, so a write after the compare
            // counts next time
            if (!c.changed && c.wr >= 0) {
                const size_t wp = (size_t)(c.host - WriteWatch::base(c.wr)) >> 12;
                if (WriteWatch::watch(c.wr, wp)) c.seen = WriteWatch::gen(c.wr, wp);
                else c.wr = -1;
            }
            // only the pieces instructions were decoded from matter
            bool changed = c.changed;
            for (uint64_t m = c.used; m && !changed; m &= m - 1) {
                const unsigned o = 64 * (unsigned)__builtin_ctzll(m);
                changed = memcmp(c.snap.get() + o, c.host + o, 64) != 0;
            }
            if (!changed) {
                if (c.wr == -1) quiet = false;  // not watched: compared every time
                ++it;
                continue;
            }
            const uint64_t pg = it->first;
            // its entries, and one at the end of the page before (4 bytes across)
            Hart::Decoded* e = &h.dc[((pg << 12) >> 1) & (Hart::DC_SIZE - 1)];
            for (unsigned i = 0; i < 2048; i++)
                if (((e[i].pc ^ h.mmuTag) >> 12) == pg) e[i].pc = ~0ull;
            Hart::Decoded& last = h.dc[(((pg << 12) - 2) >> 1) & (Hart::DC_SIZE - 1)];
            if (((last.pc ^ h.mmuTag) >> 1) == (((pg << 12) - 2) >> 1)) last.pc = ~0ull;
            h.fencePages++;
            h.codeChanged.push_back(pg);  // (at most CODE_PAGES)
            any = true;
            it = h.codePages.erase(it);
        }
        if (any) h.dcGen++;
        h.fenceQuiet = quiet;
        h.fenceWrites = tw;
    }
    // Run translated code from pc until something needs the fast loop.
    void jitRun(Hart& h, uint64_t* r, uint64_t& pc, uint64_t& left, uint8_t* cp, uint64_t cb, uint64_t span,
                uint64_t sspan, uint64_t th) {
        Jit* jp = jitFor(h);
        if (!jp) { jit = false; return; }
        Jit& J = *jp;
        jitSync(h, J);
        JitCtx c{r, left, cp, cb, span, pc, 0, sspan, th, 0, J.jt, nullptr, nullptr, 0,
                 (uint8_t*)&h.resv, &h.resvAddr, &h.mstatus};
        uint8_t* code = J.find(pc);
        if (!code && !(code = jitCompile(J, h, pc))) return;
        for (;;) {
            c.reason = 0;
            c.link = 0;
            J.enter(&c, code);
            if (c.reason == 2) {  // the block is longer than the budget left
                if (!c.left) break;
                auto it = J.steps.find(c.pc);
                uint8_t* sc = it != J.steps.end() ? it->second : jitCompile(J, h, c.pc, true);
                if (!sc) break;
                c.reason = 0;
                c.link = 0;
                J.enter(&c, sc);
            }
            if (c.reason) break;
            const uint64_t fl = J.flushes;
            code = J.find(c.pc);
            if (!code && !(code = jitCompile(J, h, c.pc))) break;
            if (c.link && J.flushes == fl) Jit::patch(c.link, code);
        }
        pc = c.pc;
        left = c.left;
    }
    // The same for supervisor or user code with paging on: blocks are found
    // by virtual pc through the fetch page record (which has the page
    // translated and allowed), each translated from the physical address
    // the pc has now; loads and stores go through the data page caches.
    void jitRunPg(Hart& h, uint64_t* r, uint64_t& pc, uint64_t& left) {
        Jit* vp_ = jitFor(h);
        Jit* jp = vp_ ? jitForPg(h) : nullptr;
        if (!jp) { jit = false; return; }
        Jit& J = *jp;   // blocks
        Jit& V = *vp_;  // this hart's indirect-jump table
        jitSync(h, V, true);
        if (V.pgFpGen != h.fpGen || V.pgPriv != h.priv) {
            Jit::clearJt(V.jtPg, V.jtPgUsed);
            V.pgFpGen = h.fpGen;
            V.pgPriv = h.priv;
            h.fpRegions.clear();
        }
        if (!h.fpRegions.empty()) {  // fetch pages dropped by SFENCE.VMA with an address
            // whichever is shorter: the slots in use, or each 4 KiB page's 2048
            bool big = V.jtPgUsed.size() < 2048 * h.fpRegions.size();
            for (const Hart::Super& rg : h.fpRegions) {
                if (rg.sh != 12) { big = true; continue; }
                if (big) continue;
                // a 4 KiB page: its 2048 slots in the table
                for (uint64_t k = 0; k < 2048; k++) {
                    Jit::JtEntry& e = V.jtPg[((rg.key << 11) + k) & (Jit::JT - 1)];
                    if (e.pc >> 12 == rg.key) e = {~0ull, nullptr};
                }
            }
            if (big)
                for (uint32_t i : V.jtPgUsed) {
                    Jit::JtEntry& e = V.jtPg[i];
                    if (e.pc == ~0ull) continue;
                    for (const Hart::Super& rg : h.fpRegions)
                        if ((e.pc >> rg.sh) == rg.key) { e = {~0ull, nullptr}; break; }
                }
            h.fpRegions.clear();
        }
        static_assert(sizeof(Hart::DataPage) == 16 && offsetof(Hart::DataPage, tag) == 0, "the translator's page cache lookup");
        JitCtx c{r, left, nullptr, 0, 0, pc, 0, 0, 0, 0, V.jtPg, h.dl, h.ds, (uint64_t)dataCtx(h) << 52,
                 (uint8_t*)&h.resv, &h.resvAddr, &h.mstatus};
        auto look = [&](uint64_t v, bool stepv) -> uint8_t* {
            if (!stepv) {
                const Jit::JtEntry& t = V.jtPg[(v >> 1) & (Jit::JT - 1)];
                if (t.pc == v) return t.code;
            }
            const Hart::FetchPage& e = h.fp[(v >> 12) & (Hart::FP_SIZE - 1)];
            if (e.vpn != v >> 12 || e.priv != h.priv) return nullptr;
            const uint64_t pa = e.pa | (v & 0xfff);
            auto it = J.pg.find(Jit::PgKey{v, pa});
            uint8_t* code = nullptr;
            if (it != J.pg.end() && it->second.pa == pa) code = stepv ? it->second.step : it->second.code;
            // translated the second time it is looked for: a pc where a
            // turn or an interrupt left off once does not get a block
            if (!code && !stepv && ++J.pgCold[v] < 2) return nullptr;
            if (!code) code = jitCompile(J, h, v, stepv, true, pa);
            if (code && !stepv) V.setJtPg(v, code);
            return code;
        };
        // A load or store that missed the page cache: fill it and go back
        // into the block (the access again), unless the page cannot be
        // cached (a device, a fault, across two pages: the interpreter's).
        auto fill = [&] {
            while ((c.reason & 0xff) == 3) {
                const uint64_t a = c.miss;
                const bool st = c.reason >> 8 & 1;
                const unsigned nbytes = (unsigned)(c.reason >> 16);
                uint8_t* p = nullptr;
                Acc r = Acc::Ok;
                const uint64_t fl = J.flushes;
                if ((a & 0xfff) + nbytes > 4096 || !dataPage(h, a, st, p, r) || !p || J.flushes != fl) {
                    c.reason = 1;
                    return;
                }
                c.reason = 0;
                c.link = 0;
                J.enter(&c, c.resume);
            }
        };
        uint8_t* code = look(pc, false);
        if (!code) return;
        for (;;) {
            c.reason = 0;
            c.link = 0;
            J.enter(&c, code);
            fill();
            if (c.reason == 2) {  // the block is longer than the budget left
                if (!c.left) break;
                uint8_t* sc = look(c.pc, true);
                if (!sc) break;
                c.reason = 0;
                c.link = 0;
                J.enter(&c, sc);
            }
            if (c.reason) break;
            const uint64_t fl = J.flushes;
            code = look(c.pc, false);
            if (!code) break;
            if (c.link && J.flushes == fl) Jit::patch(c.link, code);
        }
        pc = c.pc;
        left = c.left;
    }
    // PG: a hart in supervisor or user mode with paging on. Instructions
    // come from pages in the fetch page record, loads and stores go through
    // the data page caches; anything else leaves.
    template <bool RV64, bool PG> void turbo(Hart& h, uint64_t end) {
        // A copy of the registers with a sink, r[32], that writes to x0 go to.
        uint64_t r[33];
        memcpy(r, h.x, sizeof h.x);
        const Hart::Decoded* const dc = h.dc.get();
        const uint64_t mt = h.mmuTag;  // the decoded cache's epoch
        const uint64_t cb = h.cBase, th = tohost;
        // An access of up to 8 bytes at ad is in the cached region when
        // ad - cb < span (one compare; the last 7 bytes take the general
        // path). Stores also need the region writable and to miss tohost.
        const uint64_t region = h.cEnd > cb ? h.cEnd - cb : 0;
        const uint64_t span = region >= 8 ? region - 7 : 0;
        const uint64_t sspan = h.cRw ? span : 0;
        uint8_t* const cp = h.cPtr;
        uint64_t pc = h.pc, cyc = h.cycle;
        const uint64_t cyc0 = cyc;
        auto sx = [](uint64_t v) { return RV64 ? v : (uint64_t)(int64_t)(int32_t)v; };
        auto ax = [](uint64_t v) { return RV64 ? v : (uint64_t)(uint32_t)v; };
        auto w = [](uint64_t v) { return (uint64_t)(int64_t)(int32_t)(uint32_t)v; };
        constexpr unsigned SM = RV64 ? 63 : 31;
        const uint32_t ctx = PG ? dataCtx(h) : 0;
        const unsigned priv = h.priv;
        // Where a load or store of n bytes at ad goes in host memory, or null.
        auto ldp = [&](uint64_t ad, unsigned n) -> uint8_t* {
            if constexpr (PG) {
                const Hart::DataPage& e = h.dl[(ad >> 12) & (Hart::DP_SIZE - 1)];
                return e.tag == Hart::dpTag(ad + n - 1, ctx) ? e.host + (ad & 0xfff) : nullptr;
            } else {
                (void)n;
                return ad - cb < span ? cp + (ad - cb) : nullptr;
            }
        };
        auto stp = [&](uint64_t ad, unsigned n) -> uint8_t* {
            if constexpr (PG) {
                const Hart::DataPage& e = h.ds[(ad >> 12) & (Hart::DP_SIZE - 1)];
                return e.tag == Hart::dpTag(ad + n - 1, ctx) ? e.host + (ad & 0xfff) : nullptr;
            } else {
                return ad - cb < sspan && !(th && ad - th + n - 1 < 8 + n - 1) ? cp + (ad - cb) : nullptr;
            }
        };
        // Is the page holding pc one instructions may come from? Then poff
        // takes its pc to its physical address, which the decoded cache is
        // indexed and tagged by.
        uint64_t poff = 0;
        auto fetchOk = [&](uint64_t a) {
            const Hart::FetchPage& e = h.fp[(a >> 12) & (Hart::FP_SIZE - 1)];
            if (e.vpn != a >> 12 || e.priv != priv) return false;
            poff = e.pa - (a & ~0xfffull);
            return true;
        };
        uint64_t fpage = PG ? ~0ull : 0;  // the page last found in the record
#define DCE(v) (&dc[(((v) + poff) >> 1) & (Hart::DC_SIZE - 1)])
        // Each handler fetches the next instruction and jumps to its handler
        // itself (computed goto).
        static void* const tbl[] = {&&out, &&L_LUI, &&L_AUIPC, &&L_JAL, &&L_JALR, &&L_BEQ, &&L_BNE, &&L_BLT, &&L_BGE, &&L_BLTU, &&L_BGEU, &&L_LB, &&L_LH, &&L_LW, &&L_LD, &&L_LBU, &&L_LHU, &&L_LWU, &&L_SB, &&L_SH, &&L_SW, &&L_SD, &&L_ADDI, &&L_SLLI, &&L_SLTI, &&L_SLTIU, &&L_XORI, &&L_SRLI, &&L_SRAI, &&L_ORI, &&L_ANDI, &&L_ADD, &&L_SUB, &&L_SLL, &&L_SLT, &&L_SLTU, &&L_XOR, &&L_SRL, &&L_SRA, &&L_OR, &&L_AND, &&L_MUL, &&L_ADDIW, &&L_SLLIW, &&L_SRLIW, &&L_SRAIW, &&L_ADDW, &&L_SUBW, &&L_SLLW, &&L_SRLW, &&L_SRAW, &&L_MULW};
        const Hart::Decoded* d;
        uint64_t npc, a, b;
        int64_t i;
        uint64_t* o;
#define DISPATCH                                                         \
        do {                                                             \
            if (PG && (pc ^ fpage) >> 12) {                              \
                if (!fetchOk(pc)) goto out;                              \
                fpage = pc;                                              \
                d = DCE(pc);                                             \
            }                                                            \
            if (d->pc != (((pc + poff) | PG) ^ mt)) goto out;            \
            npc = ax(pc + d->len);                                       \
            a = r[d->rs1]; b = r[d->rs2]; i = d->imm; o = &r[d->trd];    \
            goto *tbl[d->kind];                                          \
        } while (0)
        // After a jump or branch: look pc up.
#define NEXT                                                             \
        do {                                                             \
            pc = npc;                                                    \
            if (--left == 0) goto out;                                   \
            goto blockTry;                                               \
        } while (0)
        // Straight-line: the next instruction's entry follows this one's.
#define SEQ                                                              \
        do {                                                             \
            pc = npc;                                                    \
            if (--left == 0) goto out;                                   \
            d = (const Hart::Decoded*)((const char*)d + d->step);        \
            DISPATCH;                                                    \
        } while (0)
        // Block mode: no pc or budget check between the ops of a block.
        // Each handler reads its own operands (A1, B2, IM).
#define BOP goto *op->h
#define A1 r[op->rs1]
#define B2 r[op->rs2]
#define IM ((int64_t)op->imm)
#define BNEXT do { op++; BOP; } while (0)
        // The block's last op went to npc.
#define BEND do { left -= bk->n; pc = npc; if (!left) goto out; goto blockTry; } while (0)
#define BPC (ax((bk->pc & ~1ull) + 2 * (uint64_t)op->off))
        if (cyc >= end) return;
        uint64_t left = end - cyc;  // instructions to go, this one included
        static const void* const btbl[] = {nullptr, &&B_LUI, &&B_AUIPC, &&B_JAL, &&B_JALR, &&B_BEQ, &&B_BNE, &&B_BLT, &&B_BGE, &&B_BLTU, &&B_BGEU, &&B_LB, &&B_LH, &&B_LW, &&B_LD, &&B_LBU, &&B_LHU, &&B_LWU, &&B_SB, &&B_SH, &&B_SW, &&B_SD, &&B_ADDI, &&B_SLLI, &&B_SLTI, &&B_SLTIU, &&B_XORI, &&B_SRLI, &&B_SRAI, &&B_ORI, &&B_ANDI, &&B_ADD, &&B_SUB, &&B_SLL, &&B_SLT, &&B_SLTU, &&B_XOR, &&B_SRL, &&B_SRA, &&B_OR, &&B_AND, &&B_MUL, &&B_ADDIW, &&B_SLLIW, &&B_SRLIW, &&B_SRAIW, &&B_ADDW, &&B_SUBW, &&B_SLLW, &&B_SRLW, &&B_SRAW, &&B_MULW};
        if (!h.blocks) {
            h.blocks.reset((Hart::Block*)std::calloc(Hart::BLOCKS, sizeof(Hart::Block)));
            if (!h.blocks) throw std::bad_alloc();
        }
        Hart::Block* const blocks = h.blocks.get();
        const uint32_t gen = h.dcGen;
        Hart::Block* bk;
        const Hart::BlockOp* op;
    blockTry:
#if MVPLAT_JIT
        if constexpr (RV64 && !PG) {
            if (jit) {
                jitRun(h, r, pc, left, cp, cb, span, sspan, th);
                if (!left) goto out;
                d = DCE(pc);
                DISPATCH;
            }
        }
        if constexpr (RV64 && PG) {
            if (jit) {
                jitRunPg(h, r, pc, left);
                if (!left) goto out;
                if ((pc ^ fpage) >> 12) {
                    if (!fetchOk(pc)) goto out;
                    fpage = pc;
                }
                d = DCE(pc);
                DISPATCH;
            }
        }
#endif
        // At a jump target: run a whole block when enough of the budget is
        // left for its longest, else one instruction at a time.
        if (left > Hart::BLOCK_OPS) {
            if (PG && (pc ^ fpage) >> 12) {
                if (!fetchOk(pc)) goto out;
                fpage = pc;
            }
            bk = &blocks[(pc >> 1) & (Hart::BLOCKS - 1)];
            if (bk->pc != (pc | PG) || bk->gen != gen || (PG && bk->poff != poff)) {
                // Build it from the decoded-instruction cache.
                unsigned n = 0;
                uint64_t p = pc;
                bool ctl = false;
                while (n < Hart::BLOCK_OPS) {
                    const Hart::Decoded& e = *DCE(p);
                    if (e.pc != (((p + poff) | PG) ^ mt) || !e.kind || p - pc > 2 * 255 || (PG && (p ^ pc) >> 12)) break;
                    Hart::BlockOp& q = bk->ops[n++];
                    q.h = btbl[e.kind]; q.rd = e.trd; q.rs1 = e.rs1; q.rs2 = e.rs2;
                    q.off = (uint8_t)((p - pc) >> 1); q.imm = e.imm;
                    if (e.kind == K_JAL || (e.kind >= K_BEQ && e.kind <= K_BGEU)) bk->target = ax(p + e.imm);
                    p = ax(p + e.len);
                    if (e.kind >= K_JAL && e.kind <= K_BGEU) { ctl = true; break; }
                }
                if (!n) { bk->pc = ~0ull; d = DCE(pc); DISPATCH; }
                if (!ctl) bk->ops[n++] = Hart::BlockOp{&&B_END, 32, 0, 0, (uint8_t)((p - pc) >> 1), 0};
                bk->pc = pc | PG; bk->end = p; bk->n = ctl ? n : n - 1; bk->gen = gen; bk->poff = poff;
            }
            op = bk->ops;
            BOP;
        }
        d = DCE(pc);
        DISPATCH;
    L_LUI: *o = (uint64_t)i; SEQ;
    L_AUIPC: *o = sx(pc + (uint64_t)i); SEQ;
    L_JAL: *o = sx(npc); npc = ax(pc + i); NEXT;
    L_JALR: *o = sx(npc); npc = ax((a + i) & ~1ull); NEXT;
    L_BEQ: if (a == b) { npc = ax(pc + i); NEXT; } SEQ;
    L_BNE: if (a != b) { npc = ax(pc + i); NEXT; } SEQ;
    L_BLT: if ((int64_t)a < (int64_t)b) { npc = ax(pc + i); NEXT; } SEQ;
    L_BGE: if ((int64_t)a >= (int64_t)b) { npc = ax(pc + i); NEXT; } SEQ;
    L_BLTU: if (a < b) { npc = ax(pc + i); NEXT; } SEQ;
    L_BGEU: if (a >= b) { npc = ax(pc + i); NEXT; } SEQ;
#define TLOAD(L, T, N)                                                   \
    L: {                                                                 \
        const uint64_t ad = ax(a + i);                                   \
        const uint8_t* const hp = ldp(ad, N);                            \
        if (!hp) goto out;                                                \
        T v;                                                             \
        memcpy(&v, hp, N);                                               \
        *o = sx((uint64_t)v);                                            \
        SEQ;                                                             \
    }
    TLOAD(L_LB, int8_t, 1)
    TLOAD(L_LH, int16_t, 2)
    TLOAD(L_LW, int32_t, 4)
    TLOAD(L_LD, uint64_t, 8)
    TLOAD(L_LBU, uint8_t, 1)
    TLOAD(L_LHU, uint16_t, 2)
    TLOAD(L_LWU, uint32_t, 4)
#undef TLOAD
#define TSTORE(L, T, N)                                                  \
    L: {                                                                 \
        const uint64_t ad = ax(a + i);                                   \
        uint8_t* const hp = stp(ad, N);                                  \
        if (!hp) goto out;                                                \
        const T v = (T)b;                                                \
        memcpy(hp, &v, N);                                               \
        SEQ;                                                             \
    }
    TSTORE(L_SB, uint8_t, 1)
    TSTORE(L_SH, uint16_t, 2)
    TSTORE(L_SW, uint32_t, 4)
    TSTORE(L_SD, uint64_t, 8)
#undef TSTORE
    L_ADDI: *o = sx(a + i); SEQ;
    L_SLLI: *o = sx(a << i); SEQ;
    L_SLTI: *o = (int64_t)a < i; SEQ;
    L_SLTIU: *o = a < (uint64_t)i; SEQ;
    L_XORI: *o = a ^ i; SEQ;
    L_SRLI: *o = RV64 ? a >> i : sx((uint32_t)a >> i); SEQ;
    L_SRAI: *o = RV64 ? (uint64_t)((int64_t)a >> i) : sx((uint64_t)((int32_t)a >> i)); SEQ;
    L_ORI: *o = a | i; SEQ;
    L_ANDI: *o = a & i; SEQ;
    L_ADD: *o = sx(a + b); SEQ;
    L_SUB: *o = sx(a - b); SEQ;
    L_SLL: *o = sx(a << (b & SM)); SEQ;
    L_SLT: *o = (int64_t)a < (int64_t)b; SEQ;
    L_SLTU: *o = a < b; SEQ;
    L_XOR: *o = a ^ b; SEQ;
    L_SRL: *o = RV64 ? a >> (b & SM) : sx((uint32_t)a >> (b & SM)); SEQ;
    L_SRA: *o = RV64 ? (uint64_t)((int64_t)a >> (b & SM)) : sx((uint64_t)((int32_t)a >> (b & SM))); SEQ;
    L_OR: *o = a | b; SEQ;
    L_AND: *o = a & b; SEQ;
    L_MUL: *o = sx(a * b); SEQ;
    L_ADDIW: *o = w(a + i); SEQ;
    L_SLLIW: *o = w((uint32_t)a << i); SEQ;
    L_SRLIW: *o = w((uint32_t)a >> i); SEQ;
    L_SRAIW: *o = w((int32_t)a >> i); SEQ;
    L_ADDW: *o = w(a + b); SEQ;
    L_SUBW: *o = w(a - b); SEQ;
    L_SLLW: *o = w((uint32_t)a << (b & 31)); SEQ;
    L_SRLW: *o = w((uint32_t)a >> (b & 31)); SEQ;
    L_SRAW: *o = w((int32_t)a >> (b & 31)); SEQ;
    L_MULW: *o = w(a * b); SEQ;
    // ---- block-mode handlers
    B_END: npc = bk->end; BEND;
    B_LUI: r[op->rd] = (uint64_t)IM; BNEXT;
    B_AUIPC: r[op->rd] = sx(BPC + (uint64_t)IM); BNEXT;
    B_JAL: r[op->rd] = sx(bk->end); npc = bk->target; BEND;
    B_JALR: npc = ax((A1 + IM) & ~1ull); r[op->rd] = sx(bk->end); BEND;
    B_BEQ: npc = A1 == B2 ? bk->target : bk->end; BEND;
    B_BNE: npc = A1 != B2 ? bk->target : bk->end; BEND;
    B_BLT: npc = (int64_t)A1 < (int64_t)B2 ? bk->target : bk->end; BEND;
    B_BGE: npc = (int64_t)A1 >= (int64_t)B2 ? bk->target : bk->end; BEND;
    B_BLTU: npc = A1 < B2 ? bk->target : bk->end; BEND;
    B_BGEU: npc = A1 >= B2 ? bk->target : bk->end; BEND;
#define BLOAD(L, T, N)                                                   \
    L: {                                                                 \
        const uint64_t ad = ax(A1 + IM);                                 \
        const uint8_t* const hp = ldp(ad, N);                            \
        if (!hp) goto bout;                                                \
        T v;                                                             \
        memcpy(&v, hp, N);                                               \
        r[op->rd] = sx((uint64_t)v);                                     \
        BNEXT;                                                           \
    }
    BLOAD(B_LB, int8_t, 1)
    BLOAD(B_LH, int16_t, 2)
    BLOAD(B_LW, int32_t, 4)
    BLOAD(B_LD, uint64_t, 8)
    BLOAD(B_LBU, uint8_t, 1)
    BLOAD(B_LHU, uint16_t, 2)
    BLOAD(B_LWU, uint32_t, 4)
#undef BLOAD
#define BSTORE(L, T, N)                                                  \
    L: {                                                                 \
        const uint64_t ad = ax(A1 + IM);                                 \
        uint8_t* const hp = stp(ad, N);                                  \
        if (!hp) goto bout;                                                \
        const T v = (T)B2;                                               \
        memcpy(hp, &v, N);                                               \
        BNEXT;                                                           \
    }
    BSTORE(B_SB, uint8_t, 1)
    BSTORE(B_SH, uint16_t, 2)
    BSTORE(B_SW, uint32_t, 4)
    BSTORE(B_SD, uint64_t, 8)
#undef BSTORE
    B_ADDI: r[op->rd] = sx(A1 + IM); BNEXT;
    B_SLLI: r[op->rd] = sx(A1 << IM); BNEXT;
    B_SLTI: r[op->rd] = (int64_t)A1 < IM; BNEXT;
    B_SLTIU: r[op->rd] = A1 < (uint64_t)IM; BNEXT;
    B_XORI: r[op->rd] = A1 ^ IM; BNEXT;
    B_SRLI: r[op->rd] = RV64 ? A1 >> IM : sx((uint32_t)A1 >> IM); BNEXT;
    B_SRAI: r[op->rd] = RV64 ? (uint64_t)((int64_t)A1 >> IM) : sx((uint64_t)((int32_t)A1 >> IM)); BNEXT;
    B_ORI: r[op->rd] = A1 | IM; BNEXT;
    B_ANDI: r[op->rd] = A1 & IM; BNEXT;
    B_ADD: r[op->rd] = sx(A1 + B2); BNEXT;
    B_SUB: r[op->rd] = sx(A1 - B2); BNEXT;
    B_SLL: r[op->rd] = sx(A1 << (B2 & SM)); BNEXT;
    B_SLT: r[op->rd] = (int64_t)A1 < (int64_t)B2; BNEXT;
    B_SLTU: r[op->rd] = A1 < B2; BNEXT;
    B_XOR: r[op->rd] = A1 ^ B2; BNEXT;
    B_SRL: r[op->rd] = RV64 ? A1 >> (B2 & SM) : sx((uint32_t)A1 >> (B2 & SM)); BNEXT;
    B_SRA: r[op->rd] = RV64 ? (uint64_t)((int64_t)A1 >> (B2 & SM)) : sx((uint64_t)((int32_t)A1 >> (B2 & SM))); BNEXT;
    B_OR: r[op->rd] = A1 | B2; BNEXT;
    B_AND: r[op->rd] = A1 & B2; BNEXT;
    B_MUL: r[op->rd] = sx(A1 * B2); BNEXT;
    B_ADDIW: r[op->rd] = w(A1 + IM); BNEXT;
    B_SLLIW: r[op->rd] = w((uint32_t)A1 << IM); BNEXT;
    B_SRLIW: r[op->rd] = w((uint32_t)A1 >> IM); BNEXT;
    B_SRAIW: r[op->rd] = w((int32_t)A1 >> IM); BNEXT;
    B_ADDW: r[op->rd] = w(A1 + B2); BNEXT;
    B_SUBW: r[op->rd] = w(A1 - B2); BNEXT;
    B_SLLW: r[op->rd] = w((uint32_t)A1 << (B2 & 31)); BNEXT;
    B_SRLW: r[op->rd] = w((uint32_t)A1 >> (B2 & 31)); BNEXT;
    B_SRAW: r[op->rd] = w((int32_t)A1 >> (B2 & 31)); BNEXT;
    B_MULW: r[op->rd] = w(A1 * B2); BNEXT;
    bout:
        // An op the fast loop cannot finish: stop before it.
        pc = BPC;
        left -= (uint64_t)(op - bk->ops);
        goto out;
#undef BPC
#undef BEND
#undef BNEXT
#undef BOP
#undef A1
#undef B2
#undef IM
#undef SEQ
#undef NEXT
#undef DISPATCH
#undef DCE
    out:
        memcpy(h.x + 1, r + 1, sizeof h.x - sizeof h.x[0]);
        h.pc = pc;
        h.cycle = end - left;
        h.instret += h.cycle - cyc0;
    }

    void finishSlow(Hart& h, uint64_t pc, int cls, int wrd, uint64_t wval, bool memAcc, uint64_t memA,
                    uint64_t memV, bool memW, unsigned memN) {
        if (trace) {
            char b[128];
            int w = h.rv64 ? 16 : 8;
            if (!h.rv64 && wrd < 32) wval = (uint32_t)wval;
            h.traceBuf += h.traceHead;
            if (wrd >= 32) { snprintf(b, sizeof b, " f%-2d 0x%016llx", wrd - 32, (unsigned long long)wval); h.traceBuf += b; }
            else if (wrd > 0) { snprintf(b, sizeof b, " x%-2d 0x%0*llx", wrd, w, (unsigned long long)wval); h.traceBuf += b; }
            if (memAcc) {
                if (memW) snprintf(b, sizeof b, " mem 0x%0*llx 0x%0*llx", w, (unsigned long long)memA, (int)(2 * memN),
                                   (unsigned long long)memV);
                else snprintf(b, sizeof b, " mem 0x%0*llx", w, (unsigned long long)memA);
                h.traceBuf += b;
            }
            h.traceBuf += '\n';
        }
        if (profile) {
            h.nClass[cls]++;
            h.pcHist[pc]++;
        }
    }
    void breakReservations(Hart& h, uint64_t a) {
        for (auto& o : harts)
            if (o.get() != &h && o->resv && (o->resvAddr & ~7ull) == (a & ~7ull)) o->resv = false;
    }

    bool exit_ = false;
    int exitCode_ = 0;
    bool merge_ = false;            // run() merged quanta (one hart, nothing timed)
    uint64_t cut_ = UINT64_MAX;     // where merged quanta end after a device access
    std::vector<std::unique_ptr<Jit>> jits_;  // by hart id, made on first use
    std::unique_ptr<Jit> pgShared_;           // -serial: paged code, for every hart (jitForPg)
    bool irqDirty_ = false;
    uint64_t gcycle_ = 0;
    uint64_t consoleBytes_ = 0;
    std::unique_ptr<par::Pool> pool_;
    std::chrono::steady_clock::time_point wall0_, wall1_;

    bool anyLive() const {
        for (auto& d : bus.devices())
            if (d->live()) return true;
        return false;
    }
    bool hasTimedDevice() const {
        for (auto& d : bus.devices())
            if (d->live() || d->nextEventPs(nowPs()) != UINT64_MAX) return true;
        return false;
    }
    uint64_t nextEventCycle() const {
        uint64_t ev = UINT64_MAX;
        if (clint)
            for (auto& h : harts)
                if (!(h->mip & MIP_MTIP)) ev = std::min(ev, clint->cycleOf(clint->mtimecmp[h->id]));
        for (auto& d : bus.devices()) {
            uint64_t t = d->nextEventPs(nowPs());
            if (t != UINT64_MAX) ev = std::min(ev, (t + cycle_ps - 1) / cycle_ps);
        }
        return ev;
    }

    void flushTrace() {
        if (!trace) return;
        for (auto& h : harts) {
            if (!h->traceBuf.empty()) fwrite(h->traceBuf.data(), 1, h->traceBuf.size(), trace);
            h->traceBuf.clear();
        }
    }

    // One hart: everything is ordered, so nothing waits except the RTL.
    void runOne(Hart& h, uint64_t end) {
        while (h.cycle < end && h.cycle < cut_ && !exit_) {
            if (h.wfi) { h.cycle = std::min(end, cut_); break; }
            Stop s = exec(h, end);
            flushTrace();
            if (irqDirty_) updateIrqs();
            if (s == Stop::Pending) {
                if (!cosim) throw std::runtime_error("internal: an access is pending with no bridge to service it");
                stall(h, cosim->service(h.cycle * cycle_ps));
                continue;
            }
            if (s == Stop::Break) breakHart(h);
        }
    }

    // The hart waited for the RTL until `ps`.
    void stall(Hart& h, uint64_t ps) {
        uint64_t c = (ps + cycle_ps - 1) / cycle_ps;
        if (c > h.cycle) h.cycle = c;
    }

    void breakHart(Hart& h) {
        if (!h.stopSig) h.stopSig = 5;
        if (dbg) dbg->stopped(h.id);
        h.stopSig = 0;
        h.watchKind = 0;
    }

    // Several harts, -serial: each in turn runs to the end of the quantum.
    // A hart's LR reservation does not outlast its turn (the others' stores
    // in theirs did not break it), so an SC after another hart ran fails.
    void runSerial(uint64_t end) {
        for (auto& hp : harts) {
            Hart& h = *hp;
            if (irqDirty_) updateIrqs();
            runOne(h, end);
            h.resv = false;
            if (exit_) break;
        }
    }

    // Several harts: rounds of a parallel phase and an ordered phase.
    void runRound(uint64_t end) {
        for (;;) {
            bool any = false;
            for (auto& h : harts) {
                if (h->wfi) h->cycle = end;
                if (h->cycle < end) any = true;
            }
            if (!any || exit_) break;
            auto body = [&](size_t i) {
                Hart& h = *harts[i];
                if (h.cycle >= end || h.wfi) { h.stop = Stop::Budget; return; }
                h.par = true;
                h.stop = exec(h, end);
                h.par = false;
            };
            if (pool_) pool_->run(harts.size(), body);
            else for (size_t i = 0; i < harts.size(); i++) body(i);
            // Write the buffers back in hart order; a store breaks every other
            // hart's reservation on that page's words it touched.
            for (auto& h : harts) {
                h->ov.commit([&](uint64_t pg) {
                    for (auto& o : harts)
                        if (o.get() != h.get() && o->resv && (o->resvAddr >> 12) == pg) o->resv = false;
                });
            }
            // The ordered phase: each stopped hart's one access, in hart order.
            for (auto& hp : harts) {
                Hart& h = *hp;
                if (h.stop == Stop::Pending) {
                    for (;;) {
                        Stop s = exec(h, h.cycle + 1);
                        if (s != Stop::Pending) { if (s == Stop::Break) h.stop = Stop::Break; break; }
                        if (!cosim) throw std::runtime_error("internal: an access is pending with no bridge to service it");
                        stall(h, cosim->service(h.cycle * cycle_ps));
                    }
                }
                if (exit_) break;
            }
            flushTrace();
            if (irqDirty_) updateIrqs();
            for (auto& hp : harts)
                if (hp->stop == Stop::Break) { breakHart(*hp); hp->stop = Stop::Budget; }
        }
    }

#include "vector.inc"
};

// ------------------------------------------------------------------ exec
inline Stop Machine::exec(Hart& h, uint64_t endq) {
    const bool slow = trace || profile || !breakpoints.empty() || h.stepping || !watches.empty();
    uint64_t* x = h.x;
    const bool rv64 = h.rv64;
    for (;;) {
        const uint64_t end = endq < cut_ ? endq : cut_;
        if (h.cycle >= end) break;
        if (exit_) return Stop::Exit;
        if (h.mip & h.mie) {
            interrupt(h);
        }
        if (h.wfi) { h.cycle = end; return Stop::Budget; }
        if (!slow && !h.par && !irqDirty_ && turboReady(h)) {
            if (h.priv == 3 && !h.pmpLock && !(h.mstatus & MSTATUS_MPRV)) {
                if (rv64) turbo<true, false>(h, end); else turbo<false, false>(h, end);
                if (h.cycle >= end) break;
            } else if (h.priv != 3 && h.paging()) {
                if (rv64) turbo<true, true>(h, end); else turbo<false, true>(h, end);
                if (h.cycle >= end) break;
            }
        }
        uint64_t pc = h.pc;
        if (slow && !breakpoints.empty() && !h.skipBp && breakpoints.count(pc)) return Stop::Break;
        h.skipBp = false;

        // Fetch, from the decoded-instruction cache when it holds pc.
        uint32_t insn, raw;
        unsigned len;
        // Through the MMU the cache goes by physical address (so SFENCE.VMA
        // and satp leave it alone): translate first, from the fetch page
        // record when it holds the page. An instruction in the last halfword
        // of a page might continue on another, so it is not kept.
        const bool fx = h.priv != 3 && h.paging();
        uint64_t fa = pc;
        bool recorded = false;
        if (fx) {
            const Hart::FetchPage& fpe = h.fp[(pc >> 12) & (Hart::FP_SIZE - 1)];
            if (fpe.vpn == (pc >> 12) && fpe.priv == h.priv) {
                fa = fpe.pa | (pc & 0xfff);
                recorded = true;
            } else {
                uint64_t tv;
                uint32_t dummy = 0;
                if (uint32_t c = fetchXlat(h, pc, dummy, tv, false, 2, &fa)) { trap(h, c, tv, false); h.cycle++; continue; }
            }
        }
        const bool edge = fx && (pc & 0xfff) == 0xffe;
        Hart::Decoded& dce = h.dc[(fa >> 1) & (Hart::DC_SIZE - 1)];
        const uint64_t tag = (fa | fx) ^ h.mmuTag;
        if (dce.pc == tag && !edge) {
            insn = dce.insn; raw = dce.raw; len = dce.len;
            if (fx) {
                // a page only partly executable: the second halfword too
                if (!recorded && len == 4 && !pmpAllows(h, fa + 2, 2, PMP_X, h.priv)) {
                    trap(h, 1, h.ax(pc + 2), false); h.cycle++; continue;
                }
            } else if ((h.priv != 3 || h.pmpLock) && !bareExecOk(h, pc, len)) {
                trap(h, 1, pc, false); h.cycle++; continue;
            }
        } else {
            if (fx) {
                uint64_t tv;
                raw = 0;
                if (uint32_t c = fetchXlat(h, pc, raw, tv, true, 0)) { trap(h, c, tv, false); h.cycle++; continue; }
            } else {
                uint64_t lo;
                Acc r;
                if (pc >= h.cBase && pc + 4 <= h.cEnd &&
                    (!h.par || h.ov.empty() || (((pc & 4095) <= 4092) && !h.ov.find(pc >> 12)))) {
                    memcpy(&raw, h.cPtr + (pc - h.cBase), 4);
                    r = Acc::Ok;
                } else {
                    Device* d = bus.find(pc);
                    if (!d || !d->memory()) r = Acc::Fault;
                    else {
                        h.cBase = d->base; h.cEnd = d->base + d->size; h.cPtr = d->memory(); h.cRw = d->writable();
                        r = ramRead(h, pc, 2, lo);
                        raw = (uint32_t)lo;
                        if ((lo & 3) == 3) {
                            uint64_t hi;
                            Device* d2 = (pc + 2 < h.cEnd) ? d : bus.find(pc + 2);
                            if (!d2 || !d2->memory()) r = Acc::Fault;
                            else if (d2 == d) { ramRead(h, pc + 2, 2, hi); raw |= (uint32_t)hi << 16; }
                            else { raw |= (uint32_t)d2->memory()[pc + 2 - d2->base] << 16 | (uint32_t)d2->memory()[pc + 3 - d2->base] << 24; }
                        }
                    }
                }
                if (r == Acc::Ok && (h.priv != 3 || h.pmpLock) &&
                    !pmpAllows(h, pc, (raw & 3) == 3 ? 4 : 2, PMP_X, h.priv))
                    r = Acc::Fault;
                if (r != Acc::Ok) { trap(h, 1, pc, false); h.cycle++; continue; }
                if ((pc & 1)) { trap(h, 0, pc, false); h.cycle++; continue; }
            }
            if ((raw & 3) == 3) { insn = raw; len = 4; }
            else {
                raw &= 0xffff;
                len = 2;
                insn = rvc::expand(raw, rv64);
                if (!insn) { trap(h, 2, raw, false); h.cycle++; continue; }
            }
            dce.pc = edge ? ~0ull : tag; dce.insn = insn; dce.raw = raw; dce.len = (uint8_t)len; dce.xlat = fx;
            dce.op = insn & 0x7f; dce.rd = (insn >> 7) & 31; dce.f3 = (insn >> 12) & 7;
            dce.rs1 = (insn >> 15) & 31; dce.rs2 = (insn >> 20) & 31; dce.f7 = (uint8_t)(insn >> 25);
            dce.kind = fastKind(insn, rv64, dce.imm);
            if (fx && len == 4 && (pc & 0xfff) > 0xffc) dce.kind = K_NONE;  // across two pages
            dce.trd = dce.rd ? dce.rd : 32;
            dce.step = (uint8_t)(len / 2 * sizeof(Hart::Decoded));
            if (!edge) noteInsn(h, fa, raw, len);
        }
        if (slow && trace) {
            int w = rv64 ? 16 : 8;
            if (len == 4) snprintf(h.traceHead, sizeof h.traceHead, "core %3u: %u 0x%0*llx (0x%08x)", h.id, h.priv, w, (unsigned long long)pc, raw);
            else snprintf(h.traceHead, sizeof h.traceHead, "core %3u: %u 0x%0*llx (0x%04x)", h.id, h.priv, w, (unsigned long long)pc, raw);
        }

        uint64_t npc = h.ax(pc + len);
        uint32_t op = dce.op, rd = dce.rd, f3 = dce.f3, rs1 = dce.rs1, rs2 = dce.rs2, f7 = dce.f7;
        int64_t immI = (int32_t)insn >> 20;
        int cls = C_ALU;
        int wrd = -1;          // register written, for the trace
        uint64_t wval = 0;
        bool memAcc = false;   // for the trace
        uint64_t memA = 0, memV = 0;
        bool memW = false;
        unsigned memN = 0;

// RD: write rd, as XLEN sees it. W: the sign-extended low word (the *W
// instructions of RV64, and every result on RV32).
#define RD(v) do { uint64_t _v = h.sx(v); if (rd) { x[rd] = _v; wrd = (int)rd; wval = _v; } } while (0)
#define W(v) ((uint64_t)(int64_t)(int32_t)(uint32_t)(v))
// FD: write the floating-point register rd, and mark the FP state dirty.
#define FD(v) do { uint64_t _v = (v); h.f[rd] = _v; h.mstatus |= MSTATUS_FS; wrd = 32 + (int)rd; wval = _v; } while (0)
#define TRAP(c, t) do { trap(h, (c), (t), false); h.cycle++; goto next_insn; } while (0)
#define ILLEGAL() TRAP(2, len == 4 ? insn : raw)
#define MEMFAIL(r, cause, a) do { if ((r) == Acc::Pending) return Stop::Pending; TRAP((r) == Acc::PageFault ? (cause) + 8 : (cause), a); } while (0)

        switch (op) {
            case 0x37: RD(W(insn & 0xfffff000u)); break;               // LUI
            case 0x17: RD(pc + W(insn & 0xfffff000u)); break;          // AUIPC
            case 0x6f: {                                               // JAL
                int64_t imm = (int32_t)(((insn >> 31) << 20) | (((insn >> 12) & 0xff) << 12) |
                                        (((insn >> 20) & 1) << 11) | (((insn >> 21) & 0x3ff) << 1)) << 11 >> 11;
                RD(npc);
                npc = h.ax(pc + imm);
                cls = C_JUMP;
                break;
            }
            case 0x67: {                                               // JALR
                if (f3) ILLEGAL();
                uint64_t t = h.ax((x[rs1] + immI) & ~1ull);
                RD(npc);
                npc = t;
                cls = C_JUMP;
                break;
            }
            case 0x63: {                                               // branches
                int64_t imm = (int32_t)(((insn >> 31) << 12) | (((insn >> 7) & 1) << 11) |
                                        (((insn >> 25) & 0x3f) << 5) | (((insn >> 8) & 0xf) << 1)) << 19 >> 19;
                uint64_t a = x[rs1], b = x[rs2];
                bool take;
                switch (f3) {
                    case 0: take = a == b; break;
                    case 1: take = a != b; break;
                    case 4: take = (int64_t)a < (int64_t)b; break;
                    case 5: take = (int64_t)a >= (int64_t)b; break;
                    case 6: take = a < b; break;
                    case 7: take = a >= b; break;
                    default: ILLEGAL();
                }
                if (take) npc = h.ax(pc + imm);
                cls = C_BRANCH;
                break;
            }
            case 0x03: {                                               // loads
                uint64_t a = h.ax(x[rs1] + immI), v;
                if (f3 == 7 || (!rv64 && (f3 == 3 || f3 == 6))) ILLEGAL();
                unsigned n = 1u << (f3 & 3);
                Acc r = load(h, a, n, v);
                if (r != Acc::Ok) MEMFAIL(r, 5, a);
                switch (f3) {
                    case 0: v = (uint64_t)(int64_t)(int8_t)v; break;
                    case 1: v = (uint64_t)(int64_t)(int16_t)v; break;
                    case 2: v = W(v); break;
                    default: break;  // LD, LBU, LHU, LWU
                }
                RD(v);
                cls = C_LOAD;
                memAcc = true; memA = a; memN = n;
                break;
            }
            case 0x23: {                                               // stores
                int64_t imm = (int32_t)(((insn >> 25) << 5) | ((insn >> 7) & 31)) << 20 >> 20;
                uint64_t a = h.ax(x[rs1] + imm);
                if (f3 > (rv64 ? 3u : 2u)) ILLEGAL();
                unsigned n = 1u << f3;
                Acc r = store(h, a, n, x[rs2]);
                if (r != Acc::Ok) MEMFAIL(r, 7, a);
                cls = C_STORE;
                memAcc = true; memW = true; memA = a; memN = n;
                memV = n == 8 ? x[rs2] : x[rs2] & ((1ull << (8 * n)) - 1);
                break;
            }
            case 0x13: {                                               // OP-IMM
                uint64_t a = x[rs1];
                unsigned sh = (insn >> 20) & (rv64 ? 63 : 31);
                uint32_t hi = rv64 ? insn >> 26 : f7;                  // funct6 on RV64
                uint32_t sra = rv64 ? 0x10 : 0x20;
                if ((f3 == 1 && hi) || (f3 == 5 && hi && hi != sra)) {
                    uint64_t br;
                    if (!bitmanip(insn, rv64, a, 0, br)) ILLEGAL();
                    RD(br);
                    break;
                }
                switch (f3) {
                    case 0: RD(a + immI); break;
                    case 1: if (hi) ILLEGAL(); RD(a << sh); break;
                    case 2: RD((int64_t)a < immI); break;
                    case 3: RD(a < (uint64_t)immI); break;
                    case 4: RD(a ^ immI); break;
                    case 5:
                        if (hi == 0) RD(rv64 ? a >> sh : (uint32_t)a >> sh);
                        else if (hi == sra) RD(rv64 ? (uint64_t)((int64_t)a >> sh) : (uint64_t)((int32_t)a >> sh));
                        else ILLEGAL();
                        break;
                    case 6: RD(a | immI); break;
                    case 7: RD(a & immI); break;
                }
                break;
            }
            case 0x1b: {                                               // OP-IMM-32 (RV64)
                if (!rv64) ILLEGAL();
                uint32_t a = (uint32_t)x[rs1], sh = (insn >> 20) & 31;
                if ((f3 == 1 && f7) || (f3 == 5 && f7 && f7 != 0x20)) {
                    uint64_t br;
                    if (!bitmanip(insn, rv64, x[rs1], 0, br)) ILLEGAL();
                    RD(br);
                    break;
                }
                switch (f3) {
                    case 0: RD(W(a + (uint32_t)immI)); break;                          // ADDIW
                    case 1: if (f7) ILLEGAL(); RD(W(a << sh)); break;                  // SLLIW
                    case 5:
                        if (f7 == 0) RD(W(a >> sh));                                   // SRLIW
                        else if (f7 == 0x20) RD(W((int32_t)a >> sh));                  // SRAIW
                        else ILLEGAL();
                        break;
                    default: ILLEGAL();
                }
                break;
            }
            case 0x33: {                                               // OP
                uint64_t a = x[rs1], b = x[rs2];
                if (f7 == 1) {
                    cls = f3 < 4 ? C_MUL : C_DIV;
                    if (!rv64) {
                        uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
                        int32_t sa = (int32_t)ua, sb = (int32_t)ub;
                        switch (f3) {
                            case 0: RD(ua * ub); break;
                            case 1: RD((uint32_t)(((int64_t)sa * sb) >> 32)); break;
                            case 2: RD((uint32_t)(((int64_t)sa * (int64_t)(uint64_t)ub) >> 32)); break;
                            case 3: RD((uint32_t)(((uint64_t)ua * ub) >> 32)); break;
                            case 4: RD(ub == 0 ? ~0u : (sa == INT32_MIN && sb == -1) ? ua : (uint32_t)(sa / sb)); break;
                            case 5: RD(ub == 0 ? ~0u : ua / ub); break;
                            case 6: RD(ub == 0 ? ua : (sa == INT32_MIN && sb == -1) ? 0 : (uint32_t)(sa % sb)); break;
                            case 7: RD(ub == 0 ? ua : ua % ub); break;
                        }
                        break;
                    }
                    int64_t sa = (int64_t)a, sb = (int64_t)b;
                    switch (f3) {
                        case 0: RD(a * b); break;
                        case 1: RD((uint64_t)(((__int128)sa * sb) >> 64)); break;
                        case 2: RD((uint64_t)(((__int128)sa * (__int128)(unsigned __int128)b) >> 64)); break;
                        case 3: RD((uint64_t)(((unsigned __int128)a * b) >> 64)); break;
                        case 4: RD(b == 0 ? ~0ull : (sa == INT64_MIN && sb == -1) ? a : (uint64_t)(sa / sb)); break;
                        case 5: RD(b == 0 ? ~0ull : a / b); break;
                        case 6: RD(b == 0 ? a : (sa == INT64_MIN && sb == -1) ? 0 : (uint64_t)(sa % sb)); break;
                        case 7: RD(b == 0 ? a : a % b); break;
                    }
                    break;
                }
                if (f7 != 0 && !(f7 == 0x20 && (f3 == 0 || f3 == 5))) {
                    uint64_t br;
                    if (!bitmanip(insn, rv64, a, b, br)) ILLEGAL();
                    RD(br);
                    break;
                }
                unsigned sm = rv64 ? 63 : 31;
                switch (f3) {
                    case 0: RD(f7 ? a - b : a + b); break;
                    case 1: RD(a << (b & sm)); break;
                    case 2: RD((int64_t)a < (int64_t)b); break;
                    case 3: RD(a < b); break;
                    case 4: RD(a ^ b); break;
                    case 5:
                        if (rv64) RD(f7 ? (uint64_t)((int64_t)a >> (b & 63)) : a >> (b & 63));
                        else RD(f7 ? (uint64_t)((int32_t)a >> (b & 31)) : (uint64_t)((uint32_t)a >> (b & 31)));
                        break;
                    case 6: RD(a | b); break;
                    case 7: RD(a & b); break;
                }
                break;
            }
            case 0x3b: {                                               // OP-32 (RV64)
                if (!rv64) ILLEGAL();
                uint32_t a = (uint32_t)x[rs1], b = (uint32_t)x[rs2];
                if (f7 != 0 && f7 != 1 && !(f7 == 0x20 && (f3 == 0 || f3 == 5))) {
                    uint64_t br;
                    if (!bitmanip(insn, rv64, x[rs1], x[rs2], br)) ILLEGAL();
                    RD(br);
                    break;
                }
                if (f7 == 1) {
                    int32_t sa = (int32_t)a, sb = (int32_t)b;
                    cls = f3 == 0 ? C_MUL : C_DIV;
                    switch (f3) {
                        case 0: RD(W(a * b)); break;                                                          // MULW
                        case 4: RD(W(b == 0 ? ~0u : (sa == INT32_MIN && sb == -1) ? a : (uint32_t)(sa / sb))); break;  // DIVW
                        case 5: RD(W(b == 0 ? ~0u : a / b)); break;                                           // DIVUW
                        case 6: RD(W(b == 0 ? a : (sa == INT32_MIN && sb == -1) ? 0 : (uint32_t)(sa % sb))); break;   // REMW
                        case 7: RD(W(b == 0 ? a : a % b)); break;                                             // REMUW
                        default: ILLEGAL();
                    }
                    break;
                }
                switch (f3) {
                    case 0:
                        if (f7 == 0) RD(W(a + b));                            // ADDW
                        else if (f7 == 0x20) RD(W(a - b));                    // SUBW
                        else ILLEGAL();
                        break;
                    case 1: if (f7) ILLEGAL(); RD(W(a << (b & 31))); break;   // SLLW
                    case 5:
                        if (f7 == 0) RD(W(a >> (b & 31)));                    // SRLW
                        else if (f7 == 0x20) RD(W((int32_t)a >> (b & 31)));   // SRAW
                        else ILLEGAL();
                        break;
                    default: ILLEGAL();
                }
                break;
            }
            case 0x0f:                                                 // FENCE, FENCE.I
                if (f3 == 2) {                                         // Zicbom, Zicboz: CBO.*
                    const uint32_t op = insn >> 20;
                    if (rd || op > 4 || op == 3) ILLEGAL();
                    if (op != 4) {                                     // clean, flush, inval: no caches here
                        if (h.priv != 3) ILLEGAL();                    // menvcfg.CBCFE = CBIE = 0
                        cls = C_FENCE;
                        break;
                    }
                    uint64_t a = h.ax(x[rs1] & ~63ull);                // CBO.ZERO, 64-byte blocks
                    const unsigned sw = rv64 ? 8 : 4;
                    for (unsigned o = 0; o < 64; o += sw) {
                        Acc r = store(h, h.ax(a + o), sw, 0);
                        if (r != Acc::Ok) MEMFAIL(r, 7, h.ax(x[rs1]));
                    }
                    cls = C_STORE;
                    break;
                }
                if (f3 != 0 && f3 != 1) ILLEGAL();
                cls = C_FENCE;
                if (f3 == 1) fenceI(h);
                break;
            case 0x2f: {                                               // A
                uint32_t f5 = insn >> 27;
                if (f5 == 6 || f5 == 7) {                              // Zalasr: LB..LD.AQ, SB..SD.RL
                    const bool ld = f5 == 6;
                    if (f3 > 3 || (f3 == 3 && !rv64) || (ld ? !((insn >> 26) & 1) || rs2 : !((insn >> 25) & 1) || rd)) ILLEGAL();
                    const unsigned n = 1u << f3;
                    const uint64_t a = h.ax(x[rs1]);
                    cls = ld ? C_LOAD : C_STORE;
                    if (h.par) return Stop::Pending;
                    if (a & (n - 1)) TRAP(ld ? 4 : 6, a);
                    Acc r;
                    if (ld) {
                        uint64_t v = 0;
                        r = load(h, a, n, v);
                        if (r != Acc::Ok) MEMFAIL(r, 5, a);
                        RD(n == 1 ? (uint64_t)(int64_t)(int8_t)v : n == 2 ? (uint64_t)(int64_t)(int16_t)v : n == 4 ? W(v) : v);
                        memAcc = true; memW = false; memA = a; memN = n;
                    } else {
                        const uint64_t v = x[rs2];
                        r = store(h, a, n, v);
                        if (r != Acc::Ok) MEMFAIL(r, 7, a);
                        breakReservations(h, a);
                        memAcc = true; memW = true; memA = a; memN = n; memV = n == 8 ? v : v & ((1ull << (8 * n)) - 1);
                    }
                    break;
                }
                // Zabha: byte and halfword AMOs (and AMOCAS.B/H); Zacas:
                // AMOCAS.W/D/Q, the double-width one on a register pair.
                const bool cas = f5 == 0x05;
                const bool lrsc = f5 == 2 || f5 == 3;
                if (f3 > 4 || (f3 == 4 && (!rv64 || !cas)) || (f3 == 3 && !rv64 && !cas) || (f3 < 2 && lrsc)) ILLEGAL();
                const unsigned n = 1u << f3;
                uint64_t a = h.ax(x[rs1]), v;
                cls = C_AMO;
                if (h.par) return Stop::Pending;
                if (a & (n - 1)) TRAP(f5 == 2 ? 4 : 6, a);  // atomics must be aligned
                auto ext = [&](uint64_t t) {
                    return n == 1 ? (uint64_t)(int64_t)(int8_t)t : n == 2 ? (uint64_t)(int64_t)(int16_t)t : n == 4 ? W(t) : t;
                };
                const unsigned xw = rv64 ? 8 : 4;
                if (cas && n > xw) {                                   // AMOCAS on a register pair
                    if ((rd & 1) || (rs2 & 1)) ILLEGAL();
                    Acc r = amoCheck(h, a);
                    if (r != Acc::Ok) MEMFAIL(r, 7, a);
                    uint64_t lo, hi;
                    r = load(h, a, xw, lo);
                    if (r == Acc::Ok) r = load(h, a + xw, xw, hi);
                    if (r != Acc::Ok) MEMFAIL(r, 7, a);
                    lo = h.sx(lo); hi = h.sx(hi);
                    uint64_t elo = rd ? x[rd] : 0, ehi = rd ? x[rd + 1] : 0;
                    uint64_t nlo = rs2 ? x[rs2] : 0, nhi = rs2 ? x[rs2 + 1] : 0;
                    uint64_t mk = rv64 ? ~0ull : 0xffffffffull;
                    if (((lo ^ elo) & mk) == 0 && ((hi ^ ehi) & mk) == 0) {
                        r = store(h, a, xw, nlo);
                        if (r == Acc::Ok) r = store(h, a + xw, xw, nhi);
                        if (r != Acc::Ok) MEMFAIL(r, 7, a);
                        breakReservations(h, a);
                        breakReservations(h, a + xw);
                    }
                    if (rd) { x[rd] = lo; x[rd + 1] = hi; wrd = (int)rd; wval = lo; }
                    memAcc = true; memA = a; memN = xw;
                    break;
                }
                if (f5 == 2) {                                         // LR
                    if (rs2) ILLEGAL();
                    Acc r = load(h, a, n, v);
                    if (r != Acc::Ok) MEMFAIL(r, 5, a);
                    RD(ext(v));
                    h.resv = true;
                    h.resvAddr = a;
                    memAcc = true; memA = a; memN = n;
                    break;
                }
                if (f5 == 3) {                                         // SC
                    if (h.resv && h.resvAddr == a) {
                        Acc r = store(h, a, n, x[rs2]);
                        if (r != Acc::Ok) MEMFAIL(r, 7, a);
                        breakReservations(h, a);
                        RD(0);
                        memAcc = true; memW = true; memA = a; memN = n;
                        memV = n == 4 ? (uint32_t)x[rs2] : x[rs2];
                    } else {
                        RD(1);
                    }
                    h.resv = false;
                    break;
                }
                Acc r = amoCheck(h, a);
                if (r != Acc::Ok) MEMFAIL(r, 7, a);
                r = load(h, a, n, v);
                if (r != Acc::Ok) MEMFAIL(r, 7, a);
                v = ext(v);
                uint64_t b = ext(x[rs2]), w;
                switch (f5) {
                    case 0x05: w = v == ext(x[rd]) ? b : v; break;             // AMOCAS (Zacas)
                    case 0x01: w = b; break;                                   // AMOSWAP
                    case 0x00: w = v + b; break;                               // AMOADD
                    case 0x04: w = v ^ b; break;                               // AMOXOR
                    case 0x0c: w = v & b; break;                               // AMOAND
                    case 0x08: w = v | b; break;                               // AMOOR
                    case 0x10: w = (int64_t)v < (int64_t)b ? v : b; break;     // AMOMIN
                    case 0x14: w = (int64_t)v > (int64_t)b ? v : b; break;     // AMOMAX
                    case 0x18: w = v < b ? v : b; break;                       // AMOMINU
                    case 0x1c: w = v > b ? v : b; break;                       // AMOMAXU
                    default: ILLEGAL();
                }
                r = store(h, a, n, w);
                if (r != Acc::Ok) MEMFAIL(r, 7, a);
                breakReservations(h, a);
                RD(v);
                memAcc = true; memW = true; memA = a; memN = n; memV = n == 8 ? w : w & ((1ull << (8 * n)) - 1);
                break;
            }
            case 0x57:                                                 // OP-V
            case 0x77:                                                 // OP-VE (vector crypto)
            vector: {
                uint32_t vc = 0;
                uint64_t vtv = 0;
                cls = op == 0x57 || op == 0x77 ? C_ALU : op == 0x07 ? C_LOAD : C_STORE;
                switch (vexec(h, insn, vc, vtv, wrd, wval)) {
                    case VOK: break;
                    case VILL: ILLEGAL();
                    case VPEND: return Stop::Pending;
                    default: TRAP(vc, vtv);
                }
                break;
            }
            case 0x07:                                                 // FLW, FLD
            case 0x27: {                                               // FSW, FSD
                if (f3 == 0 || f3 >= 5) goto vector;                   // vector loads and stores
                if (!(h.mstatus & MSTATUS_FS) || f3 < 1 || f3 > 3) ILLEGAL();  // FLH, FSH (Zfh) too
                unsigned n = 1u << f3;
                if (op == 0x07) {
                    uint64_t a = h.ax(x[rs1] + immI), v;
                    Acc r = load(h, a, n, v);
                    if (r != Acc::Ok) MEMFAIL(r, 5, a);
                    FD(n == 2 ? fpu::box16((uint16_t)v) : n == 4 ? fpu::box((uint32_t)v) : v);
                    cls = C_LOAD;
                    memAcc = true; memA = a; memN = n;
                } else {
                    int64_t imm = (int32_t)(((insn >> 25) << 5) | ((insn >> 7) & 31)) << 20 >> 20;
                    uint64_t a = h.ax(x[rs1] + imm), v = n == 2 ? (uint16_t)h.f[rs2] : n == 4 ? (uint32_t)h.f[rs2] : h.f[rs2];
                    Acc r = store(h, a, n, v);
                    if (r != Acc::Ok) MEMFAIL(r, 7, a);
                    cls = C_STORE;
                    memAcc = true; memW = true; memA = a; memN = n; memV = v;
                }
                break;
            }
            case 0x43: case 0x47: case 0x4b: case 0x4f:                // FMADD, FMSUB, FNMSUB, FNMADD
            case 0x53: {                                               // OP-FP
                if (!(h.mstatus & MSTATUS_FS)) ILLEGAL();
                const unsigned fmt = (insn >> 25) & 3;
                if (fmt == 3) ILLEGAL();                               // Q: not here
                if (fmt == 2) {                                        // Zfh
                    const unsigned hrm = f3 == 7 ? (h.fcsr >> 5) & 7 : f3;
                    const uint16_t ha = fpu::unbox16(h.f[rs1]), hb = fpu::unbox16(h.f[rs2]);
                    uint32_t hfl = 0;
                    cls = C_ALU;
                    if (op != 0x53) {
                        static const fpu::Op fused[4] = {fpu::Op::Madd, fpu::Op::Msub, fpu::Op::Nmsub, fpu::Op::Nmadd};
                        if (hrm > 4) ILLEGAL();
                        FD(fpu::box16(fpu::harith(fused[(op >> 2) & 3], ha, hb, fpu::unbox16(h.f[(insn >> 27) & 31]), hrm, hfl)));
                    } else {
                        const unsigned f5 = insn >> 27;
                        static const fpu::Op ops[4] = {fpu::Op::Add, fpu::Op::Sub, fpu::Op::Mul, fpu::Op::Div};
                        switch (f5) {
                            case 0x00: case 0x01: case 0x02: case 0x03:
                                if (hrm > 4) ILLEGAL();
                                FD(fpu::box16(fpu::harith(ops[f5], ha, hb, 0, hrm, hfl)));
                                break;
                            case 0x0b:
                                if (hrm > 4 || rs2) ILLEGAL();
                                FD(fpu::box16(fpu::harith(fpu::Op::Sqrt, ha, 0, 0, hrm, hfl)));
                                break;
                            case 0x04:
                                if (f3 > 2) ILLEGAL();
                                FD(fpu::box16(fpu::sgnj<uint16_t>(ha, hb, f3)));
                                break;
                            case 0x05:
                                if (f3 > 1) ILLEGAL();
                                FD(fpu::box16(fpu::hminmax(ha, hb, f3, hfl)));
                                break;
                            case 0x08:                                 // FCVT.H.S, FCVT.H.D, FCVT.BF16.S (Zfbfmin)
                                if (hrm > 4) ILLEGAL();
                                if (rs2 == 8) {
                                    FD(fpu::box16(fpu::sToBf(fpu::unbox(h.f[rs1]), hrm, hfl)));
                                    break;
                                }
                                if (rs2 > 1) ILLEGAL();
                                FD(fpu::box16(fpu::d2h(rs2 ? h.f[rs1] : fpu::sToD(fpu::unbox(h.f[rs1]), hfl), hrm, hfl)));
                                break;
                            case 0x14:
                                if (f3 > 2) ILLEGAL();
                                RD(fpu::compare<double>(fpu::h2d(ha), fpu::h2d(hb), f3, hfl));
                                break;
                            case 0x18: {                               // FCVT.W/WU/L/LU.H
                                if (hrm > 4 || rs2 > 3 || (rs2 > 1 && !rv64)) ILLEGAL();
                                RD(fpu::toInt<double>(fpu::h2d(ha), hrm, !(rs2 & 1), rs2 > 1 ? 64 : 32, hfl));
                                break;
                            }
                            case 0x1a: {                               // FCVT.H.W/WU/L/LU
                                if (hrm > 4 || rs2 > 3 || (rs2 > 1 && !rv64)) ILLEGAL();
                                FD(fpu::box16(fpu::hFromInt(x[rs1], !(rs2 & 1), rs2 > 1 ? 64 : 32, hrm, hfl)));
                                break;
                            }
                            case 0x1c:                                 // FMV.X.H, FCLASS.H
                                if (rs2) ILLEGAL();
                                if (f3 == 0) RD((uint64_t)(int64_t)(int16_t)(uint16_t)h.f[rs1]);
                                else if (f3 == 1) RD(fpu::hclassify(ha));
                                else ILLEGAL();
                                break;
                            case 0x1e:                                 // FMV.H.X
                                if (rs2 || f3) ILLEGAL();
                                FD(fpu::box16((uint16_t)x[rs1]));
                                break;
                            default: ILLEGAL();
                        }
                    }
                    if (hfl) { h.fcsr |= hfl; h.mstatus |= MSTATUS_FS; }
                    break;
                }
                const bool dbl = fmt == 1;
                unsigned rm = f3 == 7 ? (h.fcsr >> 5) & 7 : f3;
                uint32_t fl = 0;
                const uint64_t ra = h.f[rs1], rb = h.f[rs2];
                auto A = [&](fpu::Op o, uint64_t c) {
                    if (rm > 4) return false;
                    if (dbl) FD(fpu::arith<double>(o, ra, rb, c, rm, fl));
                    else FD(fpu::box(fpu::arith<float>(o, fpu::unbox(ra), fpu::unbox(rb), fpu::unbox(c), rm, fl)));
                    return true;
                };
                cls = C_ALU;
                if (op != 0x53) {
                    static const fpu::Op fused[4] = {fpu::Op::Madd, fpu::Op::Msub, fpu::Op::Nmsub, fpu::Op::Nmadd};
                    if (!A(fused[(op >> 2) & 3], h.f[(insn >> 27) & 31])) ILLEGAL();
                } else {
                    const unsigned f5 = insn >> 27;
                    const uint64_t sa = fpu::unbox(ra), sb = fpu::unbox(rb);
                    switch (f5) {
                        case 0x00: if (!A(fpu::Op::Add, 0)) ILLEGAL(); break;
                        case 0x01: if (!A(fpu::Op::Sub, 0)) ILLEGAL(); break;
                        case 0x02: if (!A(fpu::Op::Mul, 0)) ILLEGAL(); break;
                        case 0x03: if (!A(fpu::Op::Div, 0)) ILLEGAL(); break;
                        case 0x0b: if (rs2 || !A(fpu::Op::Sqrt, 0)) ILLEGAL(); break;
                        case 0x04:                                     // FSGNJ, FSGNJN, FSGNJX
                            if (f3 > 2) ILLEGAL();
                            if (dbl) FD(fpu::sgnj<uint64_t>(ra, rb, f3));
                            else FD(fpu::box(fpu::sgnj<uint32_t>((uint32_t)sa, (uint32_t)sb, f3)));
                            break;
                        case 0x05:                                     // FMIN, FMAX
                            if (f3 > 1) ILLEGAL();
                            if (dbl) FD(fpu::minmax<double>(ra, rb, f3, fl));
                            else FD(fpu::box(fpu::minmax<float>((uint32_t)sa, (uint32_t)sb, f3, fl)));
                            break;
                        case 0x08:                                     // FCVT.S.D, FCVT.D.S, FCVT.S/D.H, FCVT.S.BF16
                            if (rm > 4) ILLEGAL();
                            if (rs2 == 6 && !dbl) {                    // Zfbfmin: from BFloat16, exact
                                FD(fpu::box(fpu::bfToS(fpu::unboxBf(ra), fl)));
                                break;
                            }
                            if (rs2 == 2) {                            // from half, exact
                                uint64_t d = fpu::h2d(fpu::unbox16(ra));
                                if (dbl) {
                                    if (fpu::snan64(d)) fl |= fpu::NV;
                                    FD(std::isnan(fpu::f64(d)) ? fpu::QNAN64 : d);
                                } else FD(fpu::box(fpu::dToS(d, rm, fl)));
                                break;
                            }
                            if (rs2 != (dbl ? 0u : 1u)) ILLEGAL();
                            if (dbl) FD(fpu::sToD((uint32_t)sa, fl));
                            else FD(fpu::box(fpu::dToS(ra, rm, fl)));
                            break;
                        case 0x14:                                     // FLE, FLT, FEQ
                            if (f3 > 2) ILLEGAL();
                            RD(dbl ? fpu::compare<double>(ra, rb, f3, fl) : fpu::compare<float>((uint32_t)sa, (uint32_t)sb, f3, fl));
                            break;
                        case 0x18: {                                   // FCVT.W/WU/L/LU
                            if (rm > 4 || rs2 > 3 || (rs2 > 1 && !rv64)) ILLEGAL();
                            bool sg = !(rs2 & 1);
                            unsigned b = rs2 > 1 ? 64 : 32;
                            RD(dbl ? fpu::toInt<double>(ra, rm, sg, b, fl) : fpu::toInt<float>((uint32_t)sa, rm, sg, b, fl));
                            break;
                        }
                        case 0x1a: {                                   // FCVT.S/D.W/WU/L/LU
                            if (rm > 4 || rs2 > 3 || (rs2 > 1 && !rv64)) ILLEGAL();
                            bool sg = !(rs2 & 1);
                            unsigned b = rs2 > 1 ? 64 : 32;
                            if (dbl) FD(fpu::fromInt<double>(x[rs1], sg, b, rm, fl));
                            else FD(fpu::box(fpu::fromInt<float>(x[rs1], sg, b, rm, fl)));
                            break;
                        }
                        case 0x1c:                                     // FMV.X.W/D, FCLASS
                            if (rs2) ILLEGAL();
                            if (f3 == 0) {
                                if (dbl && !rv64) ILLEGAL();
                                RD(dbl ? ra : W(ra));
                            } else if (f3 == 1) {
                                RD(dbl ? fpu::classify<double>(ra) : fpu::classify<float>((uint32_t)sa));
                            } else ILLEGAL();
                            break;
                        case 0x1e:                                     // FMV.W.X, FMV.D.X
                            if (rs2 || f3 || (dbl && !rv64)) ILLEGAL();
                            FD(dbl ? x[rs1] : fpu::box((uint32_t)x[rs1]));
                            break;
                        default: ILLEGAL();
                    }
                }
                if (fl) { h.fcsr |= fl; h.mstatus |= MSTATUS_FS; }
                break;
            }
            case 0x73: {                                               // SYSTEM
                cls = C_SYSTEM;
                if (f3 == 0) {
                    if (insn == 0x00000073) {                          // ECALL
                        TRAP(8 + h.priv, 0);
                    }
                    if (insn == 0x00100073) {                          // EBREAK
                        if (dbg) { h.stopSig = 5; return Stop::Break; }
                        TRAP(3, pc);
                    }
                    if (insn == 0x30200073) {                          // MRET
                        if (h.priv != 3) ILLEGAL();
                        uint64_t s = h.mstatus;
                        uint8_t mpp = (s >> 11) & 3;
                        s = (s & ~(uint64_t)MSTATUS_MIE) | ((s & MSTATUS_MPIE) ? (uint64_t)MSTATUS_MIE : 0);
                        s |= MSTATUS_MPIE;
                        s &= ~(uint64_t)MSTATUS_MPP;  // MPP <- U
                        if (mpp != 3) s &= ~(uint64_t)MSTATUS_MPRV;
                        h.mstatus = s;
                        h.priv = mpp;
                        npc = h.ax(h.mepc);
                        break;
                    }
                    if (insn == 0x10200073) {                          // SRET
                        if (h.priv == 0 || (h.priv == 1 && (h.mstatus & MSTATUS_TSR))) ILLEGAL();
                        uint64_t s = h.mstatus;
                        uint8_t spp = (s & MSTATUS_SPP) ? 1 : 0;
                        s = (s & ~(uint64_t)MSTATUS_SIE) | ((s & MSTATUS_SPIE) ? (uint64_t)MSTATUS_SIE : 0);
                        s |= MSTATUS_SPIE;
                        s &= ~(uint64_t)MSTATUS_SPP;
                        s &= ~(uint64_t)MSTATUS_MPRV;  // the new mode is below M
                        h.mstatus = s;
                        h.priv = spp;
                        npc = h.ax(h.sepc);
                        break;
                    }
                    if (insn == 0x10500073) {                          // WFI
                        if (h.priv == 0 || (h.priv == 1 && (h.mstatus & MSTATUS_TW))) ILLEGAL();
                        h.pc = npc;
                        h.cycle++;
                        h.instret++;
                        if (!(h.mip & h.mie)) { h.wfi = true; }
                        if (slow) finishSlow(h, pc, cls, -1, 0, false, 0, 0, false, 0);
                        if (h.wfi) { h.cycle = std::max(h.cycle, end); return Stop::Budget; }
                        continue;
                    }
                    if ((insn & 0xfe007fff) == 0x12000073) {           // SFENCE.VMA
                        if (h.priv == 0 || (h.priv == 1 && (h.mstatus & MSTATUS_TVM))) ILLEGAL();
                        if (rs1 && h.rv64 && h.paging()) h.flushTlbAt(x[rs1]);
                        else h.flushTlb();
                        break;
                    }
                    ILLEGAL();
                }
                if (f3 == 4) ILLEGAL();
                cls = C_CSR;
                unsigned csr = insn >> 20;
                uint64_t src = h.ax((f3 & 4) ? rs1 : x[rs1]);
                bool doWrite = (f3 & 3) == 1 || rs1 != 0;
                bool doRead = (f3 & 3) != 1 || rd != 0;
                uint64_t old = 0;
                if (doRead || (f3 & 3) != 1) {
                    if (!csrRead(h, csr, old)) ILLEGAL();
                    old = h.ax(old);
                } else if (((csr >> 8) & 3) > h.priv) ILLEGAL();
                if (doWrite) {
                    // In a CSRRS or CSRRC on mip only the software-writable
                    // SEIP takes part, not the PLIC's line (privileged ISA
                    // 3.1.9): a set or clear of another bit must not latch it.
                    uint64_t base = csr == 0x344 ? (old & ~(uint64_t)MIP_SEIP) | (h.mipSw & MIP_SEIP) : old;
                    uint64_t nv = (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? base | src : base & ~src;
                    if (!csrWrite(h, csr, nv)) ILLEGAL();
                }
                RD(old);
                break;
            }
            default: ILLEGAL();
        }
#undef RD
#undef W
#undef FD
#undef TRAP
#undef ILLEGAL
#undef MEMFAIL
        x[0] = 0;
        h.pc = npc;
        h.cycle++;
        h.instret++;
        if (slow) {
            finishSlow(h, pc, cls, wrd, wval, memAcc, memA, memV, memW, memN);
            if (h.watchKind) { h.stopSig = 5; return Stop::Break; }
            if (h.stepping) { h.stepping = false; h.stopSig = 5; return Stop::Break; }
        }
        // An interrupt source may have changed (a CSR write, an MMIO write):
        // the caller re-evaluates between instructions in the ordered phase.
        if (irqDirty_ && !h.par) { updateIrqs(); }
        continue;
    next_insn:
        if (h.stepping) { h.stepping = false; h.stopSig = 5; return Stop::Break; }
        continue;
    }
    return Stop::Budget;
}

}  // namespace vp
