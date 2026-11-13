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
#include <fstream>
#include <sstream>
#include <cstdio>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "bus.hpp"
#include "devices.hpp"
#include "elf.hpp"
#include "fpu.hpp"
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
        if (harts.size() > 1 && threads > 1) {
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
            else runRound(end);
            if (exit_) break;
            gcycle_ = end;
            for (auto& d : bus.devices()) d->advance(nowPs());
            updateIrqs();
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
                    case 0x14: if (f3 == 1) { r = a | (1ull << (b & (xl - 1))); return true; } return false;
                    case 0x34: if (f3 == 1) { r = a ^ (1ull << (b & (xl - 1))); return true; } return false;
                    case 0x04: if (!rv64 && f3 == 4 && rs2 == 0) { r = a & 0xffff; return true; } return false;  // zext.h
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
                if (f7 == 0x04 && f3 == 4 && rs2 == 0) { r = a & 0xffff; return true; }      // zext.h
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
        (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 8) | (1u << 12) | (1u << 18) | (1u << 20);
    // mstatus.SD: set when the FP state is dirty (FS = 3); bit XLEN-1.
    static uint64_t sd(const Hart& h) {
        return (h.mstatus & MSTATUS_FS) == MSTATUS_FS ? (h.rv64 ? 1ull << 63 : 1ull << 31) : 0;
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
            case 0x301: v = (h.rv64 ? 2ull << 62 : 1ull << 30) | MISA_EXT; return true;
            case 0x302: v = h.medeleg; return true;
            case 0x303: v = h.mideleg; return true;
            case 0x100: v = (h.mstatus & SSTATUS_MASK) | (h.rv64 ? 2ull << 32 : 0) | sd(h); return true;
            case 0x104: v = h.mie & h.mideleg; return true;
            case 0x105: v = h.stvec; return true;
            case 0x106: v = h.scounteren; return true;
            case 0x10a: v = 0; return true;  // senvcfg
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
            case 0x30a: v = 0; return true;  // menvcfg
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
                                  MSTATUS_SPP | MSTATUS_SUM | MSTATUS_MXR | MSTATUS_TVM | MSTATUS_TSR | MSTATUS_FS)) |
                            mpp << 11;
                return true;
            }
            case 0x001: if (!(h.mstatus & MSTATUS_FS)) return false; h.fcsr = (h.fcsr & ~0x1fu) | (v & 0x1f); h.mstatus |= MSTATUS_FS; return true;
            case 0x002: if (!(h.mstatus & MSTATUS_FS)) return false; h.fcsr = (h.fcsr & 0x1f) | (v & 7) << 5; h.mstatus |= MSTATUS_FS; return true;
            case 0x003: if (!(h.mstatus & MSTATUS_FS)) return false; h.fcsr = v & 0xff; h.mstatus |= MSTATUS_FS; return true;
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
                h.flushTlb();
                h.flushDecoded();
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
            case 0x30a: case 0x31a: case 0x747: case 0x757: return true;
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
                    if (w && (pte >> 54)) return Acc::PageFault;                // reserved, PBMT, N: none here
                    if (pte & 0xa) break;                                         // R or X: a leaf
                    if (pte & 0xd0) return Acc::PageFault;                        // D, A, U on a pointer
                    if (i == 0) return Acc::PageFault;
                    a = ((pte >> 10) & ppnMask) << 12;
                }
                uint64_t ppn = (pte >> 10) & ppnMask;
                if (i > 0) {
                    uint64_t low = (1ull << (ib * i)) - 1;
                    if (ppn & low) return Acc::PageFault;  // misaligned superpage
                    ppn |= vpn & low;
                }
                t.valid = true; t.vpn = vpn; t.ppn = ppn; t.pte = (uint8_t)pte;
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
        Who w{h.id, h.cycle, true};
        if (n < 8) v &= (1ull << (8 * n)) - 1;
        Acc r = d->write(a - d->base, n, v, w);
        if (r == Acc::Ok) d->nwrite++;
        return r;
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
    // The key a data page was checked under: privilege, SUM, MXR.
    static uint32_t dataCtx(const Hart& h) {
        return dataPriv(h) | (h.mstatus & MSTATUS_SUM ? 4 : 0) | (h.mstatus & MSTATUS_MXR ? 8 : 0);
    }
    // With paging on: an access within one page through the data page cache,
    // or a translation that fills it when the page is plain memory that PMP
    // allows as a whole. Returns false when the access has to take the
    // general path (a device, part of a page allowed, a write buffer).
    bool dataPage(Hart& h, uint64_t a, bool st, uint8_t*& p, Acc& r) {
        uint32_t ctx = dataCtx(h);
        Hart::DataPage& e = (st ? h.ds : h.dl)[(a >> 12) & (Hart::DP_SIZE - 1)];
        if (e.vpn == (a >> 12) && e.ctx == ctx) { p = e.host + (a & 0xfff); return true; }
        uint64_t pa;
        r = translate(h, a, st ? 1 : 0, dataPriv(h), pa);
        if (r != Acc::Ok) { p = nullptr; return true; }
        uint64_t pg = pa & ~0xfffull;
        Device* d = bus.find(pg);
        if (!d || !d->memory() || pg + 4096 > d->base + d->size || (st && !d->writable()) ||
            (tohost && (tohost >> 12) == (pa >> 12)) || !pmpAllows(h, pg, 4096, st ? PMP_W : PMP_R, dataPriv(h)))
            return false;
        e.vpn = a >> 12;
        e.ctx = ctx;
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
    uint32_t fetchXlat(Hart& h, uint64_t pc, uint32_t& raw, uint64_t& tval, bool readBits, unsigned knownLen) {
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
            }
        }
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
    bool irqDirty_ = false;
    uint64_t gcycle_ = 0;
    uint64_t consoleBytes_ = 0;
    std::unique_ptr<par::Pool> pool_;
    std::chrono::steady_clock::time_point wall0_, wall1_;

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
        while (h.cycle < end && !exit_) {
            if (h.wfi) { h.cycle = end; break; }
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
};

// ------------------------------------------------------------------ exec
inline Stop Machine::exec(Hart& h, uint64_t end) {
    const bool slow = trace || profile || !breakpoints.empty() || h.stepping || !watches.empty();
    uint64_t* x = h.x;
    const bool rv64 = h.rv64;
    while (h.cycle < end) {
        if (exit_) return Stop::Exit;
        if (h.mip & h.mie) {
            interrupt(h);
        }
        if (h.wfi) { h.cycle = end; return Stop::Budget; }
        uint64_t pc = h.pc;
        if (slow && !breakpoints.empty() && !h.skipBp && breakpoints.count(pc)) return Stop::Break;
        h.skipBp = false;

        // Fetch, from the decoded-instruction cache when it holds pc.
        uint32_t insn, raw;
        unsigned len;
        Hart::Decoded& dce = h.dc[(pc >> 1) & (Hart::DC_SIZE - 1)];
        const bool fx = h.priv != 3 && h.paging();
        if (dce.pc == pc && dce.xlat == fx) {
            insn = dce.insn; raw = dce.raw; len = dce.len;
            if (fx && (pc >> 12) == h.fpVpn && h.priv == h.fpPriv && (len == 2 || (pc & 0xfff) <= 0xffc)) {
                // the page fetched from last: translated and executable
            } else if (fx) {
                uint64_t tv;
                uint32_t dummy = 0;
                if (uint32_t c = fetchXlat(h, pc, dummy, tv, false, len)) { trap(h, c, tv, false); h.cycle++; continue; }
            } else if ((h.priv != 3 || h.pmpLock) && !pmpAllows(h, pc, len, PMP_X, h.priv)) {
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
            dce.pc = pc; dce.insn = insn; dce.raw = raw; dce.len = (uint8_t)len; dce.xlat = fx;
            dce.op = insn & 0x7f; dce.rd = (insn >> 7) & 31; dce.f3 = (insn >> 12) & 7;
            dce.rs1 = (insn >> 15) & 31; dce.rs2 = (insn >> 20) & 31; dce.f7 = (uint8_t)(insn >> 25);
            dce.immI = (int32_t)insn >> 20;
        }
        if (slow && trace) {
            int w = rv64 ? 16 : 8;
            if (len == 4) snprintf(h.traceHead, sizeof h.traceHead, "core %3u: %u 0x%0*llx (0x%08x)", h.id, h.priv, w, (unsigned long long)pc, raw);
            else snprintf(h.traceHead, sizeof h.traceHead, "core %3u: %u 0x%0*llx (0x%04x)", h.id, h.priv, w, (unsigned long long)pc, raw);
        }

        uint64_t npc = h.ax(pc + len);
        uint32_t op = dce.op, rd = dce.rd, f3 = dce.f3, rs1 = dce.rs1, rs2 = dce.rs2, f7 = dce.f7;
        int64_t immI = dce.immI;
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
                if (f3 != 0 && f3 != 1) ILLEGAL();
                cls = C_FENCE;
                if (f3 == 1) h.flushDecoded();
                break;
            case 0x2f: {                                               // A
                if (f3 != 2 && !(f3 == 3 && rv64)) ILLEGAL();
                const unsigned n = f3 == 3 ? 8 : 4;
                uint32_t f5 = insn >> 27;
                uint64_t a = h.ax(x[rs1]), v;
                cls = C_AMO;
                if (h.par) return Stop::Pending;
                if (a & (n - 1)) TRAP(f5 == 2 ? 4 : 6, a);  // atomics must be aligned
                auto ext = [&](uint64_t t) { return n == 4 ? W(t) : t; };
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
                memAcc = true; memW = true; memA = a; memN = n; memV = n == 4 ? (uint32_t)w : w;
                break;
            }
            case 0x07:                                                 // FLW, FLD
            case 0x27: {                                               // FSW, FSD
                if (!(h.mstatus & MSTATUS_FS) || (f3 != 2 && f3 != 3)) ILLEGAL();
                unsigned n = f3 == 2 ? 4 : 8;
                if (op == 0x07) {
                    uint64_t a = h.ax(x[rs1] + immI), v;
                    Acc r = load(h, a, n, v);
                    if (r != Acc::Ok) MEMFAIL(r, 5, a);
                    FD(n == 4 ? fpu::box((uint32_t)v) : v);
                    cls = C_LOAD;
                    memAcc = true; memA = a; memN = n;
                } else {
                    int64_t imm = (int32_t)(((insn >> 25) << 5) | ((insn >> 7) & 31)) << 20 >> 20;
                    uint64_t a = h.ax(x[rs1] + imm), v = n == 4 ? (uint32_t)h.f[rs2] : h.f[rs2];
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
                if (fmt > 1) ILLEGAL();                                // H and Q: not here
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
                        case 0x08:                                     // FCVT.S.D, FCVT.D.S
                            if (rm > 4 || rs2 != (dbl ? 0u : 1u)) ILLEGAL();
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
                        h.flushTlb();
                        h.flushDecoded();
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
