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
    uint32_t tohost = 0;  // HTIF: 0 when the program has no tohost symbol
    bool timeout = false;

    // -trace and -profile.
    FILE* trace = nullptr;
    bool profile = false;

    // Debugger state.
    std::set<uint32_t> breakpoints;
    struct Watch { uint32_t addr, len; int kind; };
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

    void addHarts(unsigned n, uint32_t resetPc) {
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
        for (auto& h : harts) h->pc = (uint32_t)elf.entry;
        auto it = elf.syms.find("tohost");
        if (it != elf.syms.end()) tohost = (uint32_t)it->second;
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
            uint32_t mip = h.mip & ~(MIP_MSIP | MIP_MTIP | MIP_MEIP);
            if (clint) {
                if (clint->msip[h.id]) mip |= MIP_MSIP;
                if (clint->mtimeAtCycle(h.cycle) >= clint->mtimecmp[h.id]) mip |= MIP_MTIP;
            }
            if (plic && plic->eip(h.id)) mip |= MIP_MEIP;
            h.mip = mip;
            if (h.wfi && (h.mip & h.mie)) h.wfi = false;
        }
    }

    // ------------------------------------------------------------ CSRs
    uint64_t mtime(const Hart& h) const {
        return clint ? clint->mtimeAtCycle(h.cycle) : h.cycle * cycle_ps / tick_ps;
    }
    bool csrRead(Hart& h, unsigned csr, uint32_t& v) {
        if (((csr >> 8) & 3) > h.priv) return false;
        auto counter = [&](unsigned bit) { return h.priv == 3 || ((h.mcounteren >> bit) & 1); };
        switch (csr) {
            case 0x300: v = h.mstatus; return true;
            case 0x301: v = (1u << 30) | (1u << 0) | (1u << 2) | (1u << 8) | (1u << 12) | (1u << 20); return true;  // RV32 A C I M U
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
            case 0xb00: v = (uint32_t)(h.cycle + h.cycleAdj); return true;
            case 0xb80: v = (uint32_t)((h.cycle + h.cycleAdj) >> 32); return true;
            case 0xb02: v = (uint32_t)(h.instret + h.instretAdj); return true;
            case 0xb82: v = (uint32_t)((h.instret + h.instretAdj) >> 32); return true;
            case 0xc00: if (!counter(0)) return false; v = (uint32_t)(h.cycle + h.cycleAdj); return true;
            case 0xc80: if (!counter(0)) return false; v = (uint32_t)((h.cycle + h.cycleAdj) >> 32); return true;
            case 0xc01: if (!counter(1)) return false; v = (uint32_t)mtime(h); return true;
            case 0xc81: if (!counter(1)) return false; v = (uint32_t)(mtime(h) >> 32); return true;
            case 0xc02: if (!counter(2)) return false; v = (uint32_t)(h.instret + h.instretAdj); return true;
            case 0xc82: if (!counter(2)) return false; v = (uint32_t)((h.instret + h.instretAdj) >> 32); return true;
        }
        if (csr >= 0x3a0 && csr <= 0x3a3) { v = h.pmpcfg[csr - 0x3a0]; return true; }
        if (csr >= 0x3b0 && csr <= 0x3bf) { v = h.pmpaddr[csr - 0x3b0]; return true; }
        if ((csr >= 0xb03 && csr <= 0xb1f) || (csr >= 0xb83 && csr <= 0xb9f) || (csr >= 0x323 && csr <= 0x33f)) { v = 0; return true; }
        if ((csr >= 0xc03 && csr <= 0xc1f) || (csr >= 0xc83 && csr <= 0xc9f)) {
            if (!counter(csr & 31)) return false;
            v = 0;
            return true;
        }
        return false;
    }
    bool csrWrite(Hart& h, unsigned csr, uint32_t v) {
        if (((csr >> 8) & 3) > h.priv || (csr >> 10) == 3) return false;
        switch (csr) {
            case 0x300: {
                uint32_t mpp = (v >> 11) & 3;
                if (mpp != 0 && mpp != 3) mpp = h.mstatus >> 11 & 3;  // WARL: M or U only
                h.mstatus = (v & (MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPRV | MSTATUS_TW)) | mpp << 11;
                return true;
            }
            case 0x301: return true;  // misa: fixed
            case 0x304: h.mie = v & (MIP_MSIP | MIP_MTIP | MIP_MEIP); if (h.wfi && (h.mip & h.mie)) h.wfi = false; return true;
            case 0x305: h.mtvec = v & ~2u; return true;
            case 0x306: h.mcounteren = v & 7; return true;
            case 0x310: return true;
            case 0x320: h.mcountinhibit = v & 5; return true;
            case 0x340: h.mscratch = v; return true;
            case 0x341: h.mepc = v & ~1u; return true;
            case 0x342: h.mcause = v; return true;
            case 0x343: h.mtval = v; return true;
            case 0x344: return true;  // mip: the M bits are read-only
            case 0x30a: case 0x31a: case 0x747: case 0x757: return true;
            case 0x7a0: case 0x7a1: case 0x7a2: case 0x7a3: case 0x7a5: return true;
            case 0x7a4: return false;
            case 0xb00: h.cycleAdj = (int64_t)((((h.cycle + h.cycleAdj) & ~0xffffffffull) | v) - h.cycle); return true;
            case 0xb80: h.cycleAdj = (int64_t)((((h.cycle + h.cycleAdj) & 0xffffffffull) | (uint64_t)v << 32) - h.cycle); return true;
            case 0xb02: h.instretAdj = (int64_t)((((h.instret + h.instretAdj) & ~0xffffffffull) | v) - h.instret - 1); return true;
            case 0xb82: h.instretAdj = (int64_t)((((h.instret + h.instretAdj) & 0xffffffffull) | (uint64_t)v << 32) - h.instret - 1); return true;
        }
        if (csr >= 0x3a0 && csr <= 0x3a3) {
            uint32_t& c = h.pmpcfg[csr - 0x3a0];
            uint32_t nv = 0;
            for (unsigned i = 0; i < 4; i++) {
                uint8_t old = (uint8_t)(c >> (8 * i)), b = (uint8_t)(v >> (8 * i));
                if (old & PMP_L) b = old;                          // locked: writes ignored
                else {
                    b &= PMP_R | PMP_W | PMP_X | PMP_A | PMP_L;
                    if ((b & (PMP_R | PMP_W)) == PMP_W) b &= ~PMP_W;  // W without R is reserved
                }
                nv |= (uint32_t)b << (8 * i);
            }
            c = nv;
            pmpDecode(h);
            return true;
        }
        if (csr >= 0x3b0 && csr <= 0x3bf) {
            unsigned i = csr - 0x3b0;
            if (pmpCfg(h, i) & PMP_L) return true;
            if (i < 15 && (pmpCfg(h, i + 1) & (PMP_L | PMP_A)) == (PMP_L | PMP_TOR)) return true;
            h.pmpaddr[i] = v;
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
    static uint8_t pmpCfg(const Hart& h, unsigned i) { return (uint8_t)(h.pmpcfg[i / 4] >> (8 * (i % 4))); }
    static void pmpDecode(Hart& h) {
        h.pmpN = 0;
        h.pmpLock = false;
        for (unsigned i = 0; i < 16; i++) {
            uint8_t c = pmpCfg(h, i);
            if (!(c & PMP_A)) continue;
            uint64_t a = (uint64_t)h.pmpaddr[i] << 2, lo, hi;
            if ((c & PMP_A) == PMP_TOR) { lo = i ? (uint64_t)h.pmpaddr[i - 1] << 2 : 0; hi = a; }
            else if ((c & PMP_A) == PMP_NA4) { lo = a; hi = a + 4; }
            else {
                unsigned t = 0;
                while (t < 32 && ((h.pmpaddr[i] >> t) & 1)) t++;
                uint64_t size = 8ull << t;
                lo = a & ~(size - 1);
                hi = lo + size;
            }
            if (c & PMP_L) h.pmpLock = true;
            h.pmpR[h.pmpN++] = {lo, hi, c};
        }
    }
    // Is an access of `n` bytes at `a` with permission `perm` allowed?
    static bool pmpAllows(const Hart& h, uint32_t a, unsigned n, uint8_t perm, unsigned priv) {
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
    static bool pmpData(const Hart& h, uint32_t a, unsigned n, uint8_t perm) {
        unsigned p = dataPriv(h);
        if (p == 3 && !h.pmpLock) return true;
        return pmpAllows(h, a, n, perm, p);
    }

    // ------------------------------------------------------------ traps
    void trap(Hart& h, uint32_t cause, uint32_t tval, bool intr) {
        h.mepc = h.pc;
        h.mcause = cause | (intr ? 0x80000000u : 0);
        h.mtval = tval;
        uint32_t s = h.mstatus;
        s = (s & ~MSTATUS_MPIE) | ((s & MSTATUS_MIE) ? (uint32_t)MSTATUS_MPIE : 0u);
        s &= ~MSTATUS_MIE;
        s = (s & ~MSTATUS_MPP) | (uint32_t)h.priv << 11;
        h.mstatus = s;
        h.priv = 3;
        h.resv = false;
        h.pc = (h.mtvec & ~3u) + ((h.mtvec & 1) && intr ? 4 * cause : 0);
        if (trace) {
            char b[96];
            snprintf(b, sizeof b, "core %3u: exception %s%u, epc 0x%08x, tval 0x%08x\n", h.id,
                     intr ? "interrupt " : "", cause, h.mepc, tval);
            h.traceBuf += b;
        }
    }
    // Take the highest-priority enabled pending interrupt, if any.
    bool interrupt(Hart& h) {
        uint32_t pend = h.mip & h.mie;
        if (!pend) return false;
        if (h.priv == 3 && !(h.mstatus & MSTATUS_MIE)) return false;
        uint32_t cause = (pend & MIP_MEIP) ? 11 : (pend & MIP_MSIP) ? 3 : 7;
        trap(h, cause, 0, true);
        return true;
    }

    // ------------------------------------------------------------ memory
    // The device side of an access that missed the fast path.
    Acc devRead(Hart& h, uint32_t a, unsigned n, uint32_t& v) {
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
        if (r == Acc::Ok) { v = (uint32_t)t; d->nread++; }
        return r;
    }
    Acc devWrite(Hart& h, uint32_t a, unsigned n, uint32_t v) {
        Device* d = bus.find(a);
        if (!d || a + n > d->base + d->size) return Acc::Fault;
        if (d->memory()) {
            h.cBase = d->base; h.cEnd = d->base + d->size; h.cPtr = d->memory(); h.cRw = d->writable();
            if (!h.cRw) return Acc::Fault;
            return ramWrite(h, a, n, v);
        }
        if (h.par) return Acc::Pending;
        Who w{h.id, h.cycle, true};
        Acc r = d->write(a - d->base, n, v, w);
        if (r == Acc::Ok) d->nwrite++;
        return r;
    }
    // Plain memory, through the hart's write buffer in a parallel round.
    Acc ramRead(Hart& h, uint32_t a, unsigned n, uint32_t& v) {
        const uint8_t* p = h.cPtr + (a - h.cBase);
        if (!h.par || h.ov.empty()) {
            uint32_t t = 0;
            memcpy(&t, p, n);
            v = t;
            return Acc::Ok;
        }
        uint32_t t = 0;
        if (((a & 4095) + n) <= 4096) {
            Overlay::Page* pg = h.ov.find(a >> 12);
            if (!pg) { memcpy(&t, p, n); v = t; return Acc::Ok; }
            unsigned o = a & 4095;
            for (unsigned i = 0; i < n; i++)
                t |= (uint32_t)(pg->has(o + i) ? pg->d[o + i] : p[i]) << (8 * i);
            v = t;
            return Acc::Ok;
        }
        for (unsigned i = 0; i < n; i++) {
            uint64_t ai = (uint64_t)a + i;
            Overlay::Page* pg = h.ov.find(ai >> 12);
            uint8_t b = (pg && pg->has(ai & 4095)) ? pg->d[ai & 4095] : p[i];
            t |= (uint32_t)b << (8 * i);
        }
        v = t;
        return Acc::Ok;
    }
    Acc ramWrite(Hart& h, uint32_t a, unsigned n, uint32_t v) {
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
            Overlay::Page* pg = h.ov.touch(a >> 12, h.cPtr + (((uint64_t)a & ~4095ull) - h.cBase));
            unsigned o = a & 4095;
            memcpy(pg->d + o, &v, n);
            for (unsigned i = 0; i < n; i++) pg->set(o + i);
            return Acc::Ok;
        }
        for (unsigned i = 0; i < n; i++) {
            uint64_t ai = (uint64_t)a + i;
            uint8_t* base = h.cPtr + ((ai & ~4095ull) - h.cBase);
            Overlay::Page* pg = h.ov.touch(ai >> 12, base);
            pg->d[ai & 4095] = (uint8_t)(v >> (8 * i));
            pg->set(ai & 4095);
        }
        return Acc::Ok;
    }
    // The hot path of a load or store: plain memory, nothing to check, no
    // write buffer in use. Everything else goes through loadSlow/storeSlow.
    __attribute__((always_inline)) inline Acc load(Hart& h, uint32_t a, unsigned n, uint32_t& v) {
        if (__builtin_expect(watches.empty() && !(h.priv != 3 || h.pmpLock || (h.mstatus & MSTATUS_MPRV)) &&
                             (uint64_t)a >= h.cBase && (uint64_t)a + n <= h.cEnd && (!h.par || h.ov.empty()), 1)) {
            uint32_t t = 0;
            memcpy(&t, h.cPtr + (a - h.cBase), n);
            v = t;
            return Acc::Ok;
        }
        return loadSlow(h, a, n, v);
    }
    __attribute__((always_inline)) inline Acc store(Hart& h, uint32_t a, unsigned n, uint32_t v) {
        if (__builtin_expect(watches.empty() && !(h.priv != 3 || h.pmpLock || (h.mstatus & MSTATUS_MPRV)) &&
                             (uint64_t)a >= h.cBase && (uint64_t)a + n <= h.cEnd && h.cRw && !h.par &&
                             !(tohost && a + n > tohost && a < tohost + 8), 1)) {
            memcpy(h.cPtr + (a - h.cBase), &v, n);
            return Acc::Ok;
        }
        return storeSlow(h, a, n, v);
    }
    __attribute__((noinline)) Acc loadSlow(Hart& h, uint32_t a, unsigned n, uint32_t& v) {
        if (!watches.empty()) watchCheck(h, a, n, 3);
        if ((h.priv != 3 || h.pmpLock || (h.mstatus & MSTATUS_MPRV)) && !pmpData(h, a, n, PMP_R)) return Acc::Fault;
        if ((uint64_t)a >= h.cBase && (uint64_t)a + n <= h.cEnd) return ramRead(h, a, n, v);
        return devRead(h, a, n, v);
    }
    __attribute__((noinline)) Acc storeSlow(Hart& h, uint32_t a, unsigned n, uint32_t v) {
        if (!watches.empty()) watchCheck(h, a, n, 2);
        if ((h.priv != 3 || h.pmpLock || (h.mstatus & MSTATUS_MPRV)) && !pmpData(h, a, n, PMP_W)) return Acc::Fault;
        if ((uint64_t)a >= h.cBase && (uint64_t)a + n <= h.cEnd && h.cRw) return ramWrite(h, a, n, v);
        return devWrite(h, a, n, v);
    }
    void watchCheck(Hart& h, uint32_t a, unsigned n, int kind) {
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
    void finishSlow(Hart& h, uint32_t pc, int cls, int wrd, uint32_t wval, bool memAcc, uint32_t memA,
                    uint32_t memV, bool memW, unsigned memN) {
        if (trace) {
            char b[96];
            h.traceBuf += h.traceHead;
            if (wrd > 0) { snprintf(b, sizeof b, " x%-2d 0x%08x", wrd, wval); h.traceBuf += b; }
            if (memAcc) {
                if (memW) snprintf(b, sizeof b, " mem 0x%08x 0x%0*x", memA, (int)(2 * memN), memV);
                else snprintf(b, sizeof b, " mem 0x%08x", memA);
                h.traceBuf += b;
            }
            h.traceBuf += '\n';
        }
        if (profile) {
            h.nClass[cls]++;
            h.pcHist[pc]++;
        }
    }
    void breakReservations(Hart& h, uint32_t a) {
        for (auto& o : harts)
            if (o.get() != &h && o->resv && (o->resvAddr & ~3u) == (a & ~3u)) o->resv = false;
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
    uint32_t* x = h.x;
    while (h.cycle < end) {
        if (exit_) return Stop::Exit;
        if (h.mip & h.mie) {
            interrupt(h);
        }
        if (h.wfi) { h.cycle = end; return Stop::Budget; }
        uint32_t pc = h.pc;
        if (slow && !breakpoints.empty() && !h.skipBp && breakpoints.count(pc)) return Stop::Break;
        h.skipBp = false;

        // Fetch, from the decoded-instruction cache when it holds pc.
        uint32_t insn, raw;
        unsigned len;
        Hart::Decoded& dce = h.dc[(pc >> 1) & (Hart::DC_SIZE - 1)];
        if (dce.pc == pc) {
            insn = dce.insn; raw = dce.raw; len = dce.len;
            if ((h.priv != 3 || h.pmpLock) && !pmpAllows(h, pc, len, PMP_X, h.priv)) {
                trap(h, 1, pc, false); h.cycle++; continue;
            }
        } else {
            {
                uint32_t lo;
                Acc r;
                if ((uint64_t)pc >= h.cBase && (uint64_t)pc + 4 <= h.cEnd &&
                    (!h.par || h.ov.empty() || (((pc & 4095) <= 4092) && !h.ov.find(pc >> 12)))) {
                    memcpy(&raw, h.cPtr + (pc - h.cBase), 4);
                    r = Acc::Ok;
                } else {
                    Device* d = bus.find(pc);
                    if (!d || !d->memory()) r = Acc::Fault;
                    else {
                        h.cBase = d->base; h.cEnd = d->base + d->size; h.cPtr = d->memory(); h.cRw = d->writable();
                        r = ramRead(h, pc, 2, lo);
                        raw = lo;
                        if ((lo & 3) == 3) {
                            uint32_t hi;
                            Device* d2 = (pc + 2 < h.cEnd) ? d : bus.find(pc + 2);
                            if (!d2 || !d2->memory()) r = Acc::Fault;
                            else if (d2 == d) { ramRead(h, pc + 2, 2, hi); raw |= hi << 16; }
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
                insn = rvc::expand(raw);
                if (!insn) { trap(h, 2, raw, false); h.cycle++; continue; }
            }
            dce.pc = pc; dce.insn = insn; dce.raw = raw; dce.len = (uint8_t)len;
            dce.op = insn & 0x7f; dce.rd = (insn >> 7) & 31; dce.f3 = (insn >> 12) & 7;
            dce.rs1 = (insn >> 15) & 31; dce.rs2 = (insn >> 20) & 31; dce.f7 = (uint8_t)(insn >> 25);
            dce.immI = (int32_t)insn >> 20;
        }
        if (slow && trace) {
            if (len == 4) snprintf(h.traceHead, sizeof h.traceHead, "core %3u: %u 0x%08x (0x%08x)", h.id, h.priv, pc, raw);
            else snprintf(h.traceHead, sizeof h.traceHead, "core %3u: %u 0x%08x (0x%04x)", h.id, h.priv, pc, raw);
        }

        uint32_t npc = pc + len;
        uint32_t op = dce.op, rd = dce.rd, f3 = dce.f3, rs1 = dce.rs1, rs2 = dce.rs2, f7 = dce.f7;
        int32_t immI = dce.immI;
        int cls = C_ALU;
        int wrd = -1;          // register written, for the trace
        uint32_t wval = 0;
        bool memAcc = false;   // for the trace
        uint32_t memA = 0, memV = 0;
        bool memW = false;
        unsigned memN = 0;

#define RD(v) do { uint32_t _v = (v); if (rd) { x[rd] = _v; wrd = (int)rd; wval = _v; } } while (0)
#define TRAP(c, t) do { trap(h, (c), (t), false); h.cycle++; goto next_insn; } while (0)
#define ILLEGAL() TRAP(2, len == 4 ? insn : raw)
#define MEMFAIL(r, cause, a) do { if ((r) == Acc::Pending) return Stop::Pending; TRAP(cause, a); } while (0)

        switch (op) {
            case 0x37: RD(insn & 0xfffff000u); break;                  // LUI
            case 0x17: RD(pc + (insn & 0xfffff000u)); break;           // AUIPC
            case 0x6f: {                                               // JAL
                int32_t imm = (int32_t)(((insn >> 31) << 20) | (((insn >> 12) & 0xff) << 12) |
                                        (((insn >> 20) & 1) << 11) | (((insn >> 21) & 0x3ff) << 1)) << 11 >> 11;
                RD(npc);
                npc = pc + imm;
                cls = C_JUMP;
                break;
            }
            case 0x67: {                                               // JALR
                if (f3) ILLEGAL();
                uint32_t t = (x[rs1] + immI) & ~1u;
                RD(npc);
                npc = t;
                cls = C_JUMP;
                break;
            }
            case 0x63: {                                               // branches
                int32_t imm = (int32_t)(((insn >> 31) << 12) | (((insn >> 7) & 1) << 11) |
                                        (((insn >> 25) & 0x3f) << 5) | (((insn >> 8) & 0xf) << 1)) << 19 >> 19;
                uint32_t a = x[rs1], b = x[rs2];
                bool take;
                switch (f3) {
                    case 0: take = a == b; break;
                    case 1: take = a != b; break;
                    case 4: take = (int32_t)a < (int32_t)b; break;
                    case 5: take = (int32_t)a >= (int32_t)b; break;
                    case 6: take = a < b; break;
                    case 7: take = a >= b; break;
                    default: ILLEGAL();
                }
                if (take) npc = pc + imm;
                cls = C_BRANCH;
                break;
            }
            case 0x03: {                                               // loads
                uint32_t a = x[rs1] + immI, v;
                unsigned n = (f3 & 3) == 0 ? 1 : (f3 & 3) == 1 ? 2 : 4;
                if (f3 == 3 || f3 > 5) ILLEGAL();
                Acc r = load(h, a, n, v);
                if (r != Acc::Ok) MEMFAIL(r, 5, a);
                if (f3 == 0) v = (uint32_t)(int32_t)(int8_t)v;
                else if (f3 == 1) v = (uint32_t)(int32_t)(int16_t)v;
                RD(v);
                cls = C_LOAD;
                memAcc = true; memA = a; memN = n;
                break;
            }
            case 0x23: {                                               // stores
                int32_t imm = (int32_t)(((insn >> 25) << 5) | ((insn >> 7) & 31)) << 20 >> 20;
                uint32_t a = x[rs1] + imm;
                if (f3 > 2) ILLEGAL();
                unsigned n = 1u << f3;
                Acc r = store(h, a, n, x[rs2]);
                if (r != Acc::Ok) MEMFAIL(r, 7, a);
                cls = C_STORE;
                memAcc = true; memW = true; memA = a; memN = n;
                memV = n == 4 ? x[rs2] : x[rs2] & ((1u << (8 * n)) - 1);
                break;
            }
            case 0x13: {                                               // OP-IMM
                uint32_t a = x[rs1], sh = (insn >> 20) & 31;
                switch (f3) {
                    case 0: RD(a + immI); break;
                    case 1: if (f7) ILLEGAL(); RD(a << sh); break;
                    case 2: RD((int32_t)a < immI); break;
                    case 3: RD(a < (uint32_t)immI); break;
                    case 4: RD(a ^ immI); break;
                    case 5:
                        if (f7 == 0) RD(a >> sh);
                        else if (f7 == 0x20) RD((uint32_t)((int32_t)a >> sh));
                        else ILLEGAL();
                        break;
                    case 6: RD(a | immI); break;
                    case 7: RD(a & immI); break;
                }
                break;
            }
            case 0x33: {                                               // OP
                uint32_t a = x[rs1], b = x[rs2];
                if (f7 == 1) {
                    int32_t sa = (int32_t)a, sb = (int32_t)b;
                    cls = f3 < 4 ? C_MUL : C_DIV;
                    switch (f3) {
                        case 0: RD(a * b); break;
                        case 1: RD((uint32_t)(((int64_t)sa * sb) >> 32)); break;
                        case 2: RD((uint32_t)(((int64_t)sa * (int64_t)(uint64_t)b) >> 32)); break;
                        case 3: RD((uint32_t)(((uint64_t)a * b) >> 32)); break;
                        case 4: RD(b == 0 ? ~0u : (sa == INT32_MIN && sb == -1) ? a : (uint32_t)(sa / sb)); break;
                        case 5: RD(b == 0 ? ~0u : a / b); break;
                        case 6: RD(b == 0 ? a : (sa == INT32_MIN && sb == -1) ? 0 : (uint32_t)(sa % sb)); break;
                        case 7: RD(b == 0 ? a : a % b); break;
                    }
                    break;
                }
                if (f7 != 0 && !(f7 == 0x20 && (f3 == 0 || f3 == 5))) ILLEGAL();
                switch (f3) {
                    case 0: RD(f7 ? a - b : a + b); break;
                    case 1: RD(a << (b & 31)); break;
                    case 2: RD((int32_t)a < (int32_t)b); break;
                    case 3: RD(a < b); break;
                    case 4: RD(a ^ b); break;
                    case 5: RD(f7 ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31)); break;
                    case 6: RD(a | b); break;
                    case 7: RD(a & b); break;
                }
                break;
            }
            case 0x0f:                                                 // FENCE, FENCE.I
                if (f3 != 0 && f3 != 1) ILLEGAL();
                cls = C_FENCE;
                if (f3 == 1) h.flushDecoded();
                break;
            case 0x2f: {                                               // A
                if (f3 != 2) ILLEGAL();
                uint32_t f5 = insn >> 27, a = x[rs1], v;
                cls = C_AMO;
                if (h.par) return Stop::Pending;
                if (a & 3) TRAP(f5 == 2 ? 4 : 6, a);  // atomics must be aligned
                if (f5 == 2) {                                         // LR.W
                    if (rs2) ILLEGAL();
                    Acc r = load(h, a, 4, v);
                    if (r != Acc::Ok) MEMFAIL(r, 5, a);
                    RD(v);
                    h.resv = true;
                    h.resvAddr = a;
                    memAcc = true; memA = a; memN = 4;
                    break;
                }
                if (f5 == 3) {                                         // SC.W
                    if (h.resv && h.resvAddr == a) {
                        Acc r = store(h, a, 4, x[rs2]);
                        if (r != Acc::Ok) MEMFAIL(r, 7, a);
                        breakReservations(h, a);
                        RD(0);
                        memAcc = true; memW = true; memA = a; memN = 4; memV = x[rs2];
                    } else {
                        RD(1);
                    }
                    h.resv = false;
                    break;
                }
                Acc r = load(h, a, 4, v);
                if (r != Acc::Ok) MEMFAIL(r, 7, a);
                uint32_t b = x[rs2], w;
                switch (f5) {
                    case 0x01: w = b; break;                                   // AMOSWAP
                    case 0x00: w = v + b; break;                               // AMOADD
                    case 0x04: w = v ^ b; break;                               // AMOXOR
                    case 0x0c: w = v & b; break;                               // AMOAND
                    case 0x08: w = v | b; break;                               // AMOOR
                    case 0x10: w = (int32_t)v < (int32_t)b ? v : b; break;     // AMOMIN
                    case 0x14: w = (int32_t)v > (int32_t)b ? v : b; break;     // AMOMAX
                    case 0x18: w = v < b ? v : b; break;                       // AMOMINU
                    case 0x1c: w = v > b ? v : b; break;                       // AMOMAXU
                    default: ILLEGAL();
                }
                r = store(h, a, 4, w);
                if (r != Acc::Ok) MEMFAIL(r, 7, a);
                breakReservations(h, a);
                RD(v);
                memAcc = true; memW = true; memA = a; memN = 4; memV = w;
                break;
            }
            case 0x73: {                                               // SYSTEM
                cls = C_SYSTEM;
                if (f3 == 0) {
                    if (insn == 0x00000073) {                          // ECALL
                        TRAP(h.priv == 3 ? 11 : 8, 0);
                    }
                    if (insn == 0x00100073) {                          // EBREAK
                        if (dbg) { h.stopSig = 5; return Stop::Break; }
                        TRAP(3, pc);
                    }
                    if (insn == 0x30200073) {                          // MRET
                        if (h.priv != 3) ILLEGAL();
                        uint32_t s = h.mstatus;
                        uint8_t mpp = (s >> 11) & 3;
                        s = (s & ~MSTATUS_MIE) | ((s & MSTATUS_MPIE) ? (uint32_t)MSTATUS_MIE : 0u);
                        s |= MSTATUS_MPIE;
                        s &= ~MSTATUS_MPP;  // MPP <- U
                        if (mpp != 3) s &= ~MSTATUS_MPRV;
                        h.mstatus = s;
                        h.priv = mpp;
                        npc = h.mepc;
                        break;
                    }
                    if (insn == 0x10500073) {                          // WFI
                        if (h.priv != 3 && (h.mstatus & MSTATUS_TW)) ILLEGAL();
                        h.pc = npc;
                        h.cycle++;
                        h.instret++;
                        if (!(h.mip & h.mie)) { h.wfi = true; }
                        if (slow) finishSlow(h, pc, cls, -1, 0, false, 0, 0, false, 0);
                        if (h.wfi) { h.cycle = std::max(h.cycle, end); return Stop::Budget; }
                        continue;
                    }
                    if ((insn & 0xfe007fff) == 0x12000073) break;      // SFENCE.VMA: no MMU, a no-op
                    ILLEGAL();
                }
                if (f3 == 4) ILLEGAL();
                cls = C_CSR;
                unsigned csr = insn >> 20;
                uint32_t src = (f3 & 4) ? rs1 : x[rs1];
                bool doWrite = (f3 & 3) == 1 || rs1 != 0;
                bool doRead = (f3 & 3) != 1 || rd != 0;
                uint32_t old = 0;
                if (doRead || (f3 & 3) != 1) {
                    if (!csrRead(h, csr, old)) ILLEGAL();
                } else if (((csr >> 8) & 3) > h.priv) ILLEGAL();
                if (doWrite) {
                    uint32_t nv = (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? old | src : old & ~src;
                    if (!csrWrite(h, csr, nv)) ILLEGAL();
                }
                RD(old);
                break;
            }
            default: ILLEGAL();
        }
#undef RD
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
