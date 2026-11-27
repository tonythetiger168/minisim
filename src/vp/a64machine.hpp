// a64machine.hpp - the AArch64 platform of mvplat: processor elements
// (a64.hpp), the bus with its devices, a GICv3 (a64gic.hpp), the generic
// timer and the firmware calls (PSCI 1.1 and SMCCC 1.1 over HVC or SMC).
// The processors take turns, a quantum of cycles each, one instruction a
// cycle; devices advance between quanta.
#pragma once

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "a64.hpp"
#include "a64gic.hpp"
#include "bus.hpp"

namespace vp {
namespace a64 {

class Machine : public Platform {
public:
    Bus bus;
    std::vector<std::unique_ptr<Cpu>> cpus;
    std::unique_ptr<Gicv3> gic;
    uint64_t cyclePs_ = 10000;          // 100 MHz
    uint64_t timerHz = 62500000;        // CNTFRQ_EL0
    uint64_t quantum = 10000;
    uint64_t maxCycles = UINT64_MAX;
    bool exit_ = false;
    int exitCode = 0;
    bool quiet = false;
    unsigned undefReports = 0;          // undefined instructions reported so far
    std::function<void(const std::string&)> output = [](const std::string& s) { std::cout << s << std::flush; };

    // Platform
    void setIrq(unsigned src, bool level) override { if (gic) gic->setSpi(src, level); }
    unsigned hartCount() const override { return (unsigned)cpus.size(); }
    uint64_t tickPs() const override { return std::max<uint64_t>(1, 1000000000000ull / timerHz); }
    uint64_t cyclePs() const override { return cyclePs_; }
    void requestExit(int code) override { exit_ = true; exitCode = code; }
    void irqChanged() override { if (gic) gic->update(); }
    void out(const std::string& s) override { output(s); }
    bool masterRead(uint64_t a, uint8_t& v, const Device*) override {
        Device* d = bus.find(a);
        if (!d) return false;
        uint64_t x = 0;
        if (d->read(a - d->base, 1, x, Who{}) != Acc::Ok) return false;
        v = (uint8_t)x;
        return true;
    }
    bool masterWrite(uint64_t a, uint8_t v, const Device*) override {
        Device* d = bus.find(a);
        if (!d) return false;
        return d->write(a - d->base, 1, v, Who{}) == Acc::Ok;
    }
    uint8_t* memSpan(uint64_t a, uint64_t n, bool write) override {
        Device* d = bus.find(a);
        if (!d || !d->memory() || a + n > d->base + d->size || (write && !d->writable())) return nullptr;
        return d->memory() + (a - d->base);
    }

    void addCpus(unsigned n) {
        for (unsigned i = 0; i < n; i++) {
            auto c = std::make_unique<Cpu>();
            c->id = i;
            c->mpidr = 1ull << 31 | i;  // Aff0 = the processor number
            c->off = i != 0;
            c->flushTlb();
            cpus.push_back(std::move(c));
        }
    }
    void makeGic(unsigned nspi) {
        std::vector<Cpu*> v;
        for (auto& c : cpus) v.push_back(c.get());
        gic = std::make_unique<Gicv3>(v, nspi);
        gic->reset();
    }

    // Time
    uint64_t now() const {  // the slowest running processor's cycle
        uint64_t t = UINT64_MAX;
        for (auto& c : cpus) if (!c->off) t = std::min(t, c->cycle);
        return t == UINT64_MAX ? (cpus.empty() ? 0 : cpus[0]->cycle) : t;
    }
    uint64_t counter(const Cpu& c) const {  // CNTPCT_EL0
        return (uint64_t)((unsigned __int128)c.cycle * cyclePs_ * timerHz / 1000000000000ull);
    }
    uint64_t cycleOfCount(uint64_t cnt) const {
        unsigned __int128 ps = (unsigned __int128)cnt * 1000000000000ull;
        unsigned __int128 cyc = (ps + (unsigned __int128)timerHz * cyclePs_ - 1) / ((unsigned __int128)timerHz * cyclePs_);
        return cyc > UINT64_MAX ? UINT64_MAX : (uint64_t)cyc;
    }
    bool timerFires(const Cpu& c, bool virt) const {
        const uint32_t ctl = virt ? c.cntvCtl : c.cntpCtl;
        if (!(ctl & 1)) return false;
        const uint64_t cnt = counter(c) - (virt ? c.cntvoff : 0);
        return cnt >= (virt ? c.cntvCval : c.cntpCval);
    }
    void updateTimers(Cpu& c) {
        if (!gic) return;
        gic->setPpi(c.id, 30, timerFires(c, false) && !(c.cntpCtl & 2));
        gic->setPpi(c.id, 27, timerFires(c, true) && !(c.cntvCtl & 2));
    }
    // The cycle at which this processor's timers next fire (UINT64_MAX: never).
    uint64_t nextTimer(const Cpu& c) const {
        uint64_t t = UINT64_MAX;
        if ((c.cntpCtl & 3) == 1) t = std::min(t, cycleOfCount(c.cntpCval));
        if ((c.cntvCtl & 3) == 1) t = std::min(t, cycleOfCount(c.cntvCval + c.cntvoff));
        return t;
    }

    // Runs until a processor's cycle count reaches maxCycles or the
    // software asks to stop. The return value is the exit code.
    int run() {
        for (auto& d : bus.devices()) d->start();
        for (auto& c : cpus) updateTimers(*c);
        while (!exit_) {
            const uint64_t t0 = now();
            if (t0 >= maxCycles) break;
            uint64_t end = std::min(maxCycles, t0 + quantum);
            // a device event or a timer inside the quantum ends it there
            const uint64_t nowPs = t0 * cyclePs_;
            for (auto& d : bus.devices()) {
                uint64_t e = d->nextEventPs(nowPs);
                if (e != UINT64_MAX) end = std::min(end, std::max(t0 + 1, e / cyclePs_ + 1));
            }
            for (auto& c : cpus) if (!c->off) end = std::min(end, std::max(t0 + 1, nextTimer(*c)));
            bool ran = false;
            for (auto& c : cpus) {
                if (c->off) continue;
                if (runCpu(*c, end)) ran = true;
                if (exit_) break;
            }
            const uint64_t t1 = now();
            for (auto& d : bus.devices()) d->advance(t1 * cyclePs_);
            for (auto& c : cpus) updateTimers(*c);
            if (!ran) {
                // everyone waits: skip to the next event
                uint64_t next = UINT64_MAX;
                for (auto& c : cpus) if (!c->off) next = std::min(next, nextTimer(*c));
                for (auto& d : bus.devices()) {
                    uint64_t e = d->nextEventPs(t1 * cyclePs_);
                    if (e != UINT64_MAX) next = std::min(next, e / cyclePs_ + 1);
                }
                bool live = false;
                for (auto& d : bus.devices()) live |= d->live();
                bool anyIrq = false;
                for (auto& c : cpus) anyIrq |= !c->off && c->irqLine;
                if (anyIrq) continue;
                if (next == UINT64_MAX && !live) {
                    bool anyOn = false;
                    for (auto& c : cpus) anyOn |= !c->off;
                    if (!quiet)
                        std::cerr << (anyOn ? "mvplat: every processor waits for an interrupt that cannot come\n"
                                            : "mvplat: every processor is off\n");
                    exitCode = anyOn ? 3 : 0;
                    break;
                }
                if (next == UINT64_MAX) next = t1 + quantum;
                next = std::min(next, maxCycles);
                for (auto& c : cpus) if (!c->off && c->cycle < next) c->cycle = next;
            }
        }
        for (auto& d : bus.devices()) d->finish();
        return exitCode;
    }

    uint64_t instructions() const {
        uint64_t n = 0;
        for (auto& c : cpus) n += c->insns;
        return n;
    }

    // ---------------------------------------------------------------- run
    // Runs one processor up to cycle `end`. False when it only waited.
    bool runCpu(Cpu& c, uint64_t end) {
        bool ran = false;
        while (c.cycle < end && !exit_ && !c.off) {
            if (c.wfi) {
                if (!c.irqLine && !wakeTimer(c)) { c.cycle = end; break; }
                c.wfi = false;
            }
            ran = true;
            if (c.irqLine && !(c.daif & 0x80)) { exception(c, 0, 0, c.pc, 1); }
            uint32_t insn;
            if (!fetch(c, insn)) { c.cycle++; continue; }
            const uint64_t pc0 = c.pc;
            c.pc += 4;
            exec(c, insn, pc0);
            c.cycle++;
            c.insns++;
        }
        return ran;
    }
    bool wakeTimer(Cpu& c) {
        // a WFI wakes on a pending interrupt, masked or not
        return c.irqLine;
    }

    // ------------------------------------------------------- exceptions
    // kind: 0 synchronous, 1 IRQ, 2 FIQ, 3 SError. ret: ELR_EL1.
    void exception(Cpu& c, uint32_t ec, uint32_t iss, uint64_t ret, int kind = 0) {
        c.spsr = c.pstate();
        c.elr = ret;
        if (kind == 0) c.esr = (uint64_t)ec << 26 | 1u << 25 | (iss & 0x1ffffff);
        uint64_t off = c.el == 0 ? 0x400 : (c.spsel ? 0x200 : 0x000);
        c.el = 1;
        c.spsel = true;
        c.daif = 0x3c0;
        c.pc = c.vbar + off + 0x80 * (uint64_t)kind;
        c.wfi = false;
    }
    void undefined(Cpu& c, uint32_t insn, uint64_t pc) {
        if (undefReports < 20 && !quiet && c.el) {  // EL0: the kernel handles it (SIGILL, emulated MRS)
            char b[96];
            snprintf(b, sizeof b, "mvplat: cpu %u: undefined instruction 0x%08x at 0x%llx (EL%u)\n", c.id, insn,
                     (unsigned long long)pc, c.el);
            std::cerr << b;
            if (++undefReports == 20) std::cerr << "mvplat: (no more undefined instructions reported)\n";
        }
        exception(c, EC_UNKNOWN, 0, pc);
    }

    // ------------------------------------------------------- translation
    // Fault status codes (DFSC / IFSC)
    static constexpr uint32_t F_ADDRSIZE = 0x00, F_TRANS = 0x04, F_ACCESS = 0x08, F_PERM = 0x0c, F_EXTWALK = 0x14,
                              F_ALIGN = 0x21, F_EXT = 0x10;

    bool mmuOn(const Cpu& c) const { return c.sctlr & 1; }
    // The virtual address with the tag byte replaced (TBI), for data accesses.
    static uint64_t untag(const Cpu& c, uint64_t va) {
        const bool upper = va >> 55 & 1;
        if (c.tcr >> (upper ? 38 : 37) & 1) return upper ? (va | 0xff00000000000000ull) : (va & 0x00ffffffffffffffull);
        return va;
    }
    // A walk. 0: done (pa: the page, perm, global, level); else a fault status.
    uint32_t walk(Cpu& c, uint64_t va, uint64_t& pa, uint8_t& perm, bool& global, unsigned& level) {
        const bool upper = va >> 55 & 1;
        unsigned tsz = (unsigned)(upper ? (c.tcr >> 16) : c.tcr) & 63;
        const bool epd = c.tcr >> (upper ? 23 : 7) & 1;
        const unsigned tg = (unsigned)(upper ? (c.tcr >> 30) : (c.tcr >> 14)) & 3;
        level = 0;
        if (tg != (upper ? 2u : 0u)) return F_TRANS;  // only the 4 KiB granule
        tsz = std::clamp(tsz, 16u, 39u);
        const unsigned in = 64 - tsz;
        const uint64_t top = va >> in;
        if (top != (upper ? (~0ull >> in) : 0)) return F_TRANS;
        if (epd) return F_TRANS;
        unsigned lv = in > 39 ? 0 : in > 30 ? 1 : in > 21 ? 2 : 3;
        uint64_t table = (upper ? c.ttbr1 : c.ttbr0) & 0x0000fffffffff000ull;
        bool apNoEl0 = false, apRo = false, pxnT = false, uxnT = false;
        for (;; lv++) {
            level = lv;
            const unsigned shift = 39 - 9 * lv;
            const uint64_t idx = (va >> shift) & (lv == 0 || shift + 9 <= in ? 0x1ff : ((1ull << (in - shift)) - 1));
            uint64_t d = 0;
            if (!phys(table + idx * 8, 8, d)) return F_EXTWALK + lv;
            if (!(d & 1)) return F_TRANS + lv;
            if (lv < 3 && (d & 2)) {
                pxnT |= d >> 59 & 1;
                uxnT |= d >> 60 & 1;
                apNoEl0 |= d >> 61 & 1;
                apRo |= d >> 62 & 1;
                table = d & 0x0000fffffffff000ull;
                continue;
            }
            if (lv == 3 && !(d & 2)) return F_TRANS + lv;
            if (lv == 0) return F_TRANS + lv;  // no level 0 blocks with 4 KiB pages
            if (!(d >> 10 & 1)) return F_ACCESS + lv;
            const uint64_t bs = 1ull << shift;
            pa = ((d & 0x0000fffffffff000ull & ~(bs - 1)) | (va & (bs - 1))) & ~0xfffull;
            const unsigned ap = (unsigned)(d >> 6) & 3;
            const bool el0 = (ap & 1) && !apNoEl0, ro = (ap & 2) || apRo;
            perm = 1;
            if (!ro) perm |= 2;
            if (el0) perm |= 8 | (ro ? 0 : 16);
            const bool pxn = (d >> 53 & 1) || pxnT || (el0 && !ro);  // EL0-writable is never EL1-executable
            const bool uxn = (d >> 54 & 1) || uxnT;
            const bool wxn = c.sctlr >> 19 & 1;
            if (!pxn && !(wxn && !ro)) perm |= 4;
            if (!uxn && !(wxn && el0 && !ro)) perm |= 32;
            global = !(d >> 11 & 1);
            return 0;
        }
    }
    // A physical read for the walker (RAM only).
    bool phys(uint64_t pa, unsigned n, uint64_t& v) {
        Device* d = bus.find(pa);
        if (!d || !d->memory() || pa + n > d->base + d->size) return false;
        v = 0;
        memcpy(&v, d->memory() + (pa - d->base), n);
        return true;
    }
    uint8_t* hostPage(uint64_t pa) {
        Device* d = bus.find(pa);
        if (!d || !d->memory() || pa + 4096 > d->base + d->size) return nullptr;
        return d->memory() + (pa - d->base);
    }

    // Translates va for an access (acc 0 read, 1 write, 2 fetch; asEl0: an
    // unprivileged LDTR / STTR). On a fault, takes the abort (when raise)
    // and returns false. host: the page in host memory, when it is RAM
    // (writable RAM, for a write).
    bool xlate(Cpu& c, uint64_t va, int acc, bool asEl0, uint64_t& pa, uint8_t*& host, bool raise = true,
               uint32_t* fsc = nullptr) {
        const uint64_t vaIn = va;
        if (acc != 2) va = untag(c, va);
        const bool el0 = c.el == 0 || asEl0;
        const uint8_t need = (uint8_t)((acc == 0 ? 1 : acc == 1 ? 2 : 4) << (el0 ? 3 : 0));
        if (!mmuOn(c)) {
            if (va >> 40) return abort(c, vaIn, acc, F_ADDRSIZE, raise, fsc);
            pa = va & ~0xfffull;
            host = hostPage(pa);
            if (host && acc == 1 && !bus.find(pa)->writable()) host = nullptr;
            pa |= va & 0xfff;
            return true;
        }
        Cpu::Tlb& e = c.tlb[(va >> 12) & (Cpu::TLB_N - 1)];
        const uint16_t asid = c.asid();
        if (e.tag == va >> 12 && (e.global || e.asid == asid)) {
            if (!(e.perm & need)) return abort(c, vaIn, acc, F_PERM + 3, raise, fsc);
            pa = e.pa | (va & 0xfff);
            host = acc == 1 && !(e.perm & 0x40) ? nullptr : e.host;
            return true;
        }
        uint64_t pg = 0;
        uint8_t perm = 0;
        bool global = false;
        unsigned level = 0;
        if (uint32_t f = walk(c, va, pg, perm, global, level)) return abort(c, vaIn, acc, f, raise, fsc);
        e.tag = va >> 12;
        e.pa = pg;
        e.asid = asid;
        e.global = global;
        e.host = hostPage(pg);
        if (e.host && bus.find(pg)->writable()) perm |= 0x40;  // writable RAM
        e.perm = perm;
        if (!(perm & need)) return abort(c, vaIn, acc, F_PERM + level, raise, fsc);
        pa = pg | (va & 0xfff);
        host = acc == 1 && !(perm & 0x40) ? nullptr : e.host;
        return true;
    }
    bool abort(Cpu& c, uint64_t va, int acc, uint32_t fsc, bool raise, uint32_t* out) {
        if (out) *out = fsc;
        if (!raise) return false;
        c.far = va;
        if (acc == 2) exception(c, c.el == 0 ? EC_IABT_LOW : EC_IABT_CUR, fsc, c.pc);
        else exception(c, c.el == 0 ? EC_DABT_LOW : EC_DABT_CUR, fsc | (acc == 1 ? 1u << 6 : 0), curPc_);
        return false;
    }

    // ---------------------------------------------------------- memory
    uint64_t curPc_ = 0;  // the instruction being executed (data aborts return to it)
    bool fetch(Cpu& c, uint32_t& insn) {
        if (c.pc & 3) { c.far = c.pc; exception(c, EC_PC_ALIGN, 0, c.pc); return false; }
        const uint64_t key = (c.pc >> 12) << 1 | (c.el == 0);
        Cpu::Fast& f = c.fastX[(c.pc >> 12) & (Cpu::FAST_N - 1)];
        if (f.key == key) { memcpy(&insn, f.host + (c.pc & 0xfff), 4); return true; }
        uint64_t pa;
        uint8_t* h;
        if (!xlate(c, c.pc, 2, false, pa, h)) return false;
        if (h) { f = {key, h, pa & ~0xfffull}; memcpy(&insn, h + (pa & 0xfff), 4); return true; }
        uint64_t v;
        Device* d = bus.find(pa);
        if (!d || d->read(pa - d->base, 4, v, Who{c.id, c.cycle, true}) != Acc::Ok) {
            c.far = c.pc;
            exception(c, c.el == 0 ? EC_IABT_LOW : EC_IABT_CUR, F_EXT, c.pc);
            return false;
        }
        insn = (uint32_t)v;
        return true;
    }
    bool devAccess(Cpu& c, uint64_t va, uint64_t pa, unsigned n, uint64_t& v, bool write) {
        Device* d = bus.find(pa);
        Acc r = Acc::Fault;
        if (d && pa + n <= d->base + d->size) {
            const Who w{c.id, c.cycle, true};
            r = write ? d->write(pa - d->base, n, v, w) : d->read(pa - d->base, n, v, w);
            if (write) d->nwrite++; else d->nread++;
        }
        if (r != Acc::Ok) {
            c.far = va;
            exception(c, c.el == 0 ? EC_DABT_LOW : EC_DABT_CUR, F_EXT | (write ? 1u << 6 : 0), curPc_);
            return false;
        }
        return true;
    }
    // Loads and stores of 1, 2, 4 or 8 bytes.
    bool rd(Cpu& c, uint64_t va, unsigned n, uint64_t& v, bool asEl0 = false) {
        if ((va & 0xfff) + n > 4096) {  // across two pages: a byte at a time
            uint64_t r = 0;
            for (unsigned i = 0; i < n; i++) {
                uint64_t b;
                if (!rd(c, va + i, 1, b, asEl0)) return false;
                r |= b << (8 * i);
            }
            v = r;
            return true;
        }
        const uint64_t key = (va >> 12) << 1 | (c.el == 0 || asEl0);
        Cpu::Fast& f = c.fastR[(va >> 12) & (Cpu::FAST_N - 1)];
        if (f.key == key) { v = 0; memcpy(&v, f.host + (va & 0xfff), n); return true; }
        uint64_t pa;
        uint8_t* h;
        if (!xlate(c, va, 0, asEl0, pa, h)) return false;
        if (h) {
            f = {key, h, pa & ~0xfffull};
            v = 0;
            memcpy(&v, h + (pa & 0xfff), n);
            return true;
        }
        v = 0;
        return devAccess(c, va, pa, n, v, false);
    }
    bool wr(Cpu& c, uint64_t va, unsigned n, uint64_t v, bool asEl0 = false) {
        if ((va & 0xfff) + n > 4096) {
            // translate both pages first, so a fault on the second writes nothing
            uint64_t pa;
            uint8_t* h;
            if (!xlate(c, va, 1, asEl0, pa, h) || !xlate(c, (va | 0xfff) + 1, 1, asEl0, pa, h)) return false;
            for (unsigned i = 0; i < n; i++)
                if (!wr(c, va + i, 1, v >> (8 * i) & 0xff, asEl0)) return false;
            return true;
        }
        const uint64_t key = (va >> 12) << 1 | (c.el == 0 || asEl0);
        Cpu::Fast& f = c.fastW[(va >> 12) & (Cpu::FAST_N - 1)];
        if (f.key == key) {
            if (monitors_) clearMonitors(c, f.pa | (va & 0xfff), n);
            memcpy(f.host + (va & 0xfff), &v, n);
            return true;
        }
        uint64_t pa;
        uint8_t* h;
        if (!xlate(c, va, 1, asEl0, pa, h)) return false;
        if (monitors_) clearMonitors(c, pa, n);
        if (h) {
            f = {key, h, pa & ~0xfffull};
            memcpy(h + (pa & 0xfff), &v, n);
            return true;
        }
        return devAccess(c, va, pa, n, v, true);
    }
    // The exclusives monitors: a store to a granule another processor holds
    // clears its reservation.
    unsigned monitors_ = 0;
    void clearMonitors(Cpu& self, uint64_t pa, unsigned n) {
        for (auto& o : cpus) {
            if (o.get() == &self || o->exAddr == ~0ull) continue;
            if ((pa & ~63ull) <= (o->exAddr | 63) && (o->exAddr & ~63ull) <= ((pa + n - 1) | 63)) {
                o->exAddr = ~0ull;
                monitors_--;
            }
        }
    }
    void setMonitor(Cpu& c, uint64_t pa, unsigned n) {
        if (c.exAddr == ~0ull) monitors_++;
        c.exAddr = pa;
        c.exSize = n;
    }
    void clearMonitor(Cpu& c) {
        if (c.exAddr != ~0ull) monitors_--;
        c.exAddr = ~0ull;
    }

    // ------------------------------------------------------ registers
    static uint64_t X(const Cpu& c, unsigned r) { return r == 31 ? 0 : c.x[r]; }
    static uint64_t XS(Cpu& c, unsigned r) { return r == 31 ? c.spRef() : c.x[r]; }  // SP for 31
    static void setX(Cpu& c, unsigned r, uint64_t v, bool sf) {
        if (r != 31) c.x[r] = sf ? v : (uint32_t)v;
    }
    static void setXS(Cpu& c, unsigned r, uint64_t v, bool sf) {
        if (!sf) v = (uint32_t)v;
        if (r == 31) c.spRef() = v; else c.x[r] = v;
    }
    static bool cond(const Cpu& c, unsigned cd) {
        const bool n = c.nzcv >> 31 & 1, z = c.nzcv >> 30 & 1, cy = c.nzcv >> 29 & 1, v = c.nzcv >> 28 & 1;
        bool r;
        switch (cd >> 1) {
            case 0: r = z; break;
            case 1: r = cy; break;
            case 2: r = n; break;
            case 3: r = v; break;
            case 4: r = cy && !z; break;
            case 5: r = n == v; break;
            case 6: r = n == v && !z; break;
            default: r = true; break;
        }
        return (cd & 1) && cd != 15 ? !r : r;
    }
    // AddWithCarry, setting NZCV when s.
    static uint64_t addc(Cpu& c, uint64_t a, uint64_t b, unsigned carry, bool sf, bool s) {
        uint64_t r;
        if (sf) {
            unsigned __int128 u = (unsigned __int128)a + b + carry;
            r = (uint64_t)u;
            if (s) {
                const bool cy = u >> 64;
                const bool v = ((~(a ^ b) & (a ^ r)) >> 63) & 1;
                c.nzcv = (uint32_t)((r >> 63) << 31 | (r == 0) << 30 | cy << 29 | v << 28);
            }
        } else {
            const uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
            const uint64_t u = (uint64_t)a32 + b32 + carry;
            const uint32_t r32 = (uint32_t)u;
            r = r32;
            if (s) {
                const bool cy = u >> 32;
                const bool v = ((~(a32 ^ b32) & (a32 ^ r32)) >> 31) & 1;
                c.nzcv = (uint32_t)((r32 >> 31) << 31 | (r32 == 0) << 30 | cy << 29 | v << 28);
            }
        }
        return r;
    }
    static void nzLogic(Cpu& c, uint64_t r, bool sf) {
        const bool n = sf ? r >> 63 : r >> 31 & 1;
        const bool z = sf ? r == 0 : (uint32_t)r == 0;
        c.nzcv = (uint32_t)(n << 31 | z << 30);
    }
    static uint64_t shiftReg(uint64_t v, unsigned type, unsigned amt, bool sf) {
        const unsigned w = sf ? 64 : 32;
        if (!sf) v = (uint32_t)v;
        if (!amt) return v;
        switch (type) {
            case 0: return sf ? v << amt : (uint32_t)(v << amt);
            case 1: return v >> amt;
            case 2: return sf ? (uint64_t)((int64_t)v >> amt) : (uint32_t)((int32_t)v >> amt);
            default: return sf ? (v >> amt | v << (w - amt)) : (uint32_t)((uint32_t)v >> amt | (uint32_t)v << (w - amt));
        }
    }
    static uint64_t extendReg(uint64_t v, unsigned opt, unsigned sh) {
        switch (opt) {
            case 0: v = (uint8_t)v; break;
            case 1: v = (uint16_t)v; break;
            case 2: v = (uint32_t)v; break;
            case 3: break;
            case 4: v = (uint64_t)(int64_t)(int8_t)v; break;
            case 5: v = (uint64_t)(int64_t)(int16_t)v; break;
            case 6: v = (uint64_t)(int64_t)(int32_t)v; break;
            default: break;
        }
        return v << sh;
    }
    static uint64_t ones(unsigned n) { return n >= 64 ? ~0ull : (1ull << n) - 1; }
    static uint64_t ror(uint64_t v, unsigned r, unsigned w) {
        r %= w;
        if (!r) return v;
        return ((v >> r) | (v << (w - r))) & ones(w);
    }
    static uint64_t replicate(uint64_t v, unsigned esize, unsigned w) {
        uint64_t r = 0;
        for (unsigned i = 0; i < w; i += esize) r |= v << i;
        return r & ones(w);
    }
    // DecodeBitMasks (J1.1.3.x): false when the encoding is reserved.
    static bool bitMasks(unsigned n, unsigned imms, unsigned immr, bool immediate, unsigned w, uint64_t& wmask, uint64_t& tmask) {
        const unsigned v = n << 6 | (~imms & 0x3f);
        if (!v) return false;
        int len = 31 - __builtin_clz(v);
        if (len < 1) return false;
        const unsigned levels = (unsigned)ones((unsigned)len);
        if (immediate && (imms & levels) == levels) return false;
        const unsigned s = imms & levels, r = immr & levels, diff = (s - r) & levels;
        const unsigned esize = 1u << len;
        if (esize > w) return false;
        const uint64_t welem = ones(s + 1), telem = ones(diff + 1);
        wmask = replicate(ror(welem, r, esize), esize, w);
        tmask = replicate(telem, esize, w);
        return true;
    }

    // ------------------------------------------------------ the decoder
    void exec(Cpu& c, uint32_t i, uint64_t pc);
    void dpImm(Cpu& c, uint32_t i, uint64_t pc);
    void branchSys(Cpu& c, uint32_t i, uint64_t pc);
    void loadStore(Cpu& c, uint32_t i, uint64_t pc);
    void dpReg(Cpu& c, uint32_t i, uint64_t pc);
    void simdFp(Cpu& c, uint32_t i, uint64_t pc);
    void simdLdSt(Cpu& c, uint32_t i, uint64_t pc);
    void system(Cpu& c, uint32_t i, uint64_t pc);
    bool sysReg(Cpu& c, uint32_t reg, bool write, uint64_t& v, uint32_t i, uint64_t pc);
    void sysOp(Cpu& c, uint32_t i, uint64_t pc);
    bool firmware(Cpu& c);
    bool fpAllowed(Cpu& c, uint64_t pc) {
        const unsigned fpen = (unsigned)(c.cpacr >> 20) & 3;
        if (fpen == 3 || (fpen == 1 && c.el == 1)) return true;
        exception(c, EC_FP, 0x1e00000, pc);
        return false;
    }
    // SIMD and floating-point register access
    static uint64_t vlo(const Cpu& c, unsigned r) { return (uint64_t)c.v[r]; }
    static void setV(Cpu& c, unsigned r, u128 v) { c.v[r] = v; }
};

inline void Machine::exec(Cpu& c, uint32_t i, uint64_t pc) {
    curPc_ = pc;
    switch ((i >> 25) & 0xf) {
        case 0x8: case 0x9: dpImm(c, i, pc); break;
        case 0xa: case 0xb: branchSys(c, i, pc); break;
        case 0x4: case 0x6: case 0xc: case 0xe: loadStore(c, i, pc); break;
        case 0x5: case 0xd: dpReg(c, i, pc); break;
        case 0x7: case 0xf: simdFp(c, i, pc); break;
        default: undefined(c, i, pc); break;
    }
}

// Data processing, immediate (C4.1.86)
inline void Machine::dpImm(Cpu& c, uint32_t i, uint64_t pc) {
    const bool sf = i >> 31;
    const unsigned rd = i & 31, rn = (i >> 5) & 31;
    switch ((i >> 23) & 7) {
        case 0: case 1: {  // ADR, ADRP
            int64_t imm = (int64_t)(((uint64_t)((i >> 5) & 0x7ffff) << 2 | ((i >> 29) & 3)) << 43) >> 43;
            if (i >> 31) setX(c, rd, (pc & ~0xfffull) + (uint64_t)(imm << 12), true);
            else setX(c, rd, pc + (uint64_t)imm, true);
            return;
        }
        case 2: {  // ADD/SUB (immediate)
            const bool sub = i >> 30 & 1, s = i >> 29 & 1;
            uint64_t imm = (i >> 10) & 0xfff;
            if (i >> 22 & 1) imm <<= 12;
            const uint64_t a = XS(c, rn);
            const uint64_t r = sub ? addc(c, a, ~imm, 1, sf, s) : addc(c, a, imm, 0, sf, s);
            if (s) setX(c, rd, r, sf); else setXS(c, rd, r, sf);
            return;
        }
        case 4: {  // logical (immediate)
            const unsigned n = i >> 22 & 1;
            if (!sf && n) break;
            uint64_t wm, tm;
            if (!bitMasks(n, (i >> 10) & 63, (i >> 16) & 63, true, sf ? 64 : 32, wm, tm)) break;
            const uint64_t a = X(c, rn);
            uint64_t r;
            switch ((i >> 29) & 3) {
                case 0: r = a & wm; break;
                case 1: r = a | wm; break;
                case 2: r = a ^ wm; break;
                default: r = a & wm; nzLogic(c, r, sf); setX(c, rd, r, sf); return;
            }
            setXS(c, rd, r, sf);
            return;
        }
        case 5: {  // move wide
            const unsigned opc = (i >> 29) & 3, hw = (i >> 21) & 3;
            if (opc == 1 || (!sf && hw > 1)) break;
            const uint64_t imm = (uint64_t)((i >> 5) & 0xffff) << (16 * hw);
            uint64_t r;
            if (opc == 0) r = ~imm;
            else if (opc == 2) r = imm;
            else r = (X(c, rd) & ~(0xffffull << (16 * hw))) | imm;
            setX(c, rd, r, sf);
            return;
        }
        case 6: {  // bitfield
            const unsigned opc = (i >> 29) & 3, n = i >> 22 & 1, immr = (i >> 16) & 63, imms = (i >> 10) & 63;
            const unsigned w = sf ? 64 : 32;
            if (opc == 3 || n != (unsigned)sf || (!sf && (immr > 31 || imms > 31))) break;
            uint64_t wm, tm;
            if (!bitMasks(n, imms, immr, false, w, wm, tm)) break;
            const uint64_t src = X(c, rn), dst = opc == 1 ? X(c, rd) : 0;
            const uint64_t bot = (dst & ~wm) | (ror(src & ones(w), immr, w) & wm);
            uint64_t top = opc == 0 ? ((src >> imms & 1) ? ones(w) : 0) : dst;
            uint64_t r = (top & ~tm) | (bot & tm);
            setX(c, rd, r, sf);
            return;
        }
        case 7: {  // EXTR
            const unsigned n = i >> 22 & 1, rm = (i >> 16) & 31, lsb = (i >> 10) & 63;
            if (n != (unsigned)sf || (i >> 29 & 3) || (i >> 21 & 1) || (!sf && lsb > 31)) break;
            const unsigned w = sf ? 64 : 32;
            const uint64_t hi = X(c, rn) & ones(w), lo = X(c, rm) & ones(w);
            const uint64_t r = lsb ? ((lo >> lsb) | (hi << (w - lsb))) & ones(w) : lo;
            setX(c, rd, r, sf);
            return;
        }
        default: break;
    }
    undefined(c, i, pc);
}

// Branches, exception generating and system instructions (C4.1.87)
inline void Machine::branchSys(Cpu& c, uint32_t i, uint64_t pc) {
    if ((i & 0x7c000000) == 0x14000000) {  // B, BL
        const int64_t off = (int64_t)((uint64_t)(i & 0x3ffffff) << 38) >> 36;
        if (i >> 31) c.x[30] = pc + 4;
        c.pc = pc + (uint64_t)off;
        return;
    }
    if ((i & 0xfe000000) == 0x54000000) {  // B.cond (and BC.cond)
        const int64_t off = (int64_t)((uint64_t)((i >> 5) & 0x7ffff) << 45) >> 43;
        if (cond(c, i & 15)) c.pc = pc + (uint64_t)off;
        return;
    }
    if ((i & 0x7e000000) == 0x34000000) {  // CBZ, CBNZ
        const int64_t off = (int64_t)((uint64_t)((i >> 5) & 0x7ffff) << 45) >> 43;
        uint64_t v = X(c, i & 31);
        if (!(i >> 31)) v = (uint32_t)v;
        if ((v == 0) != (bool)(i >> 24 & 1)) c.pc = pc + (uint64_t)off;
        return;
    }
    if ((i & 0x7e000000) == 0x36000000) {  // TBZ, TBNZ
        const unsigned bit = (i >> 31) << 5 | ((i >> 19) & 31);
        const int64_t off = (int64_t)((uint64_t)((i >> 5) & 0x3fff) << 50) >> 48;
        if ((X(c, i & 31) >> bit & 1) == (i >> 24 & 1)) c.pc = pc + (uint64_t)off;
        return;
    }
    if ((i & 0xff000000) == 0xd4000000) {  // exception generation
        const unsigned opc = (i >> 21) & 7, ll = i & 3, op2 = (i >> 2) & 7;
        const uint32_t imm = (i >> 5) & 0xffff;
        if (op2) { undefined(c, i, pc); return; }
        if (opc == 0 && ll == 1) { exception(c, EC_SVC, imm, pc + 4); return; }
        if (opc == 0 && (ll == 2 || ll == 3)) {  // HVC, SMC: the firmware answers from EL1
            if (c.el == 0 || !firmware(c)) undefined(c, i, pc);
            return;
        }
        if (opc == 1 && ll == 0) { exception(c, EC_BRK, imm, pc); return; }
        undefined(c, i, pc);
        return;
    }
    if ((i & 0xffc00000) == 0xd5000000) { system(c, i, pc); return; }
    if ((i & 0xfe000000) == 0xd6000000) {  // unconditional branch (register)
        const unsigned opc = (i >> 21) & 15, op2 = (i >> 16) & 31, op3 = (i >> 10) & 63, rn = (i >> 5) & 31, op4 = i & 31;
        if (op2 != 31 || op3 || op4) { undefined(c, i, pc); return; }
        if (opc == 0) { c.pc = X(c, rn); return; }                             // BR
        if (opc == 1) { const uint64_t t = X(c, rn); c.x[30] = pc + 4; c.pc = t; return; }  // BLR
        if (opc == 2) { c.pc = X(c, rn); return; }                             // RET
        if (opc == 4 && rn == 31 && c.el == 1) {                               // ERET
            const uint64_t s = c.spsr;
            const unsigned m = (unsigned)s & 0x1f;
            if ((m & 0x10) || (m >> 2) > 1 || (m & 2) || ((m >> 2) == 0 && (m & 1))) {
                // an illegal return: stay at EL1 with PSTATE.IL (taken at the next instruction)
                c.pc = c.elr;
                exception(c, EC_ILLEGAL, 0, c.pc);
                return;
            }
            c.el = m >> 2;
            c.spsel = m & 1;
            c.nzcv = (uint32_t)s & 0xf0000000u;
            c.daif = (uint32_t)s & 0x3c0;
            c.pc = c.elr;
            clearMonitor(c);
            return;
        }
        undefined(c, i, pc);
        return;
    }
    undefined(c, i, pc);
}

// Data processing, register (C4.1.89)
inline void Machine::dpReg(Cpu& c, uint32_t i, uint64_t pc) {
    const bool sf = i >> 31;
    const unsigned rd = i & 31, rn = (i >> 5) & 31, rm = (i >> 16) & 31;
    const unsigned op1 = i >> 28 & 1, op2 = (i >> 21) & 15;
    const unsigned w = sf ? 64 : 32;
    if (!op1) {
        if (!(op2 & 8)) {  // logical (shifted register)
            const unsigned sh = (i >> 22) & 3, amt = (i >> 10) & 63;
            if (!sf && amt > 31) { undefined(c, i, pc); return; }
            uint64_t b = shiftReg(X(c, rm), sh, amt, sf);
            if (i >> 21 & 1) b = ~b;
            const uint64_t a = X(c, rn);
            uint64_t r;
            switch ((i >> 29) & 3) {
                case 0: r = a & b; break;
                case 1: r = a | b; break;
                case 2: r = a ^ b; break;
                default: r = a & b; nzLogic(c, r, sf); break;
            }
            setX(c, rd, r, sf);
            return;
        }
        const bool sub = i >> 30 & 1, s = i >> 29 & 1;
        if (!(op2 & 1)) {  // add/sub (shifted register)
            const unsigned sh = (i >> 22) & 3, amt = (i >> 10) & 63;
            if (sh == 3 || (!sf && amt > 31)) { undefined(c, i, pc); return; }
            const uint64_t b = shiftReg(X(c, rm), sh, amt, sf), a = X(c, rn);
            setX(c, rd, sub ? addc(c, a, ~b, 1, sf, s) : addc(c, a, b, 0, sf, s), sf);
            return;
        }
        // add/sub (extended register)
        const unsigned opt = (i >> 13) & 7, amt = (i >> 10) & 7;
        if (((i >> 22) & 3) || amt > 4) { undefined(c, i, pc); return; }
        const uint64_t b = extendReg(X(c, rm), opt, amt), a = XS(c, rn);
        const uint64_t r = sub ? addc(c, a, ~b, 1, sf, s) : addc(c, a, b, 0, sf, s);
        if (s) setX(c, rd, r, sf); else setXS(c, rd, r, sf);
        return;
    }
    switch (op2) {
        case 0: {  // ADC, ADCS, SBC, SBCS
            if ((i >> 10) & 63) break;
            const bool sub = i >> 30 & 1, s = i >> 29 & 1;
            const uint64_t a = X(c, rn), b = X(c, rm);
            const unsigned cy = c.nzcv >> 29 & 1;
            setX(c, rd, addc(c, a, sub ? ~b : b, cy, sf, s), sf);
            return;
        }
        case 2: {  // CCMN, CCMP
            if (!(i >> 29 & 1) || (i >> 10 & 1) || (i >> 4 & 1)) break;
            const uint64_t a = X(c, rn), b = (i >> 11 & 1) ? rm : X(c, rm);
            if (cond(c, (i >> 12) & 15)) {
                if (i >> 30 & 1) addc(c, a, ~b, 1, sf, true);
                else addc(c, a, b, 0, sf, true);
            } else c.nzcv = (i & 15) << 28;
            return;
        }
        case 4: {  // CSEL, CSINC, CSINV, CSNEG
            if (i >> 29 & 1 || (i >> 11 & 1)) break;
            const unsigned o = (i >> 30 & 1) << 1 | (i >> 10 & 1);
            uint64_t r;
            if (cond(c, (i >> 12) & 15)) r = X(c, rn);
            else {
                r = X(c, rm);
                if (o == 1) r = r + 1;
                else if (o == 2) r = ~r;
                else if (o == 3) r = 0 - r;
            }
            setX(c, rd, r, sf);
            return;
        }
        case 6: {
            if (i >> 29 & 1) break;
            const unsigned opc = (i >> 10) & 63;
            if (i >> 30 & 1) {  // data processing (1 source)
                if (rm) break;
                const uint64_t a = X(c, rn) & ones(w);
                uint64_t r;
                switch (opc) {
                    case 0: {  // RBIT
                        r = 0;
                        for (unsigned b = 0; b < w; b++) if (a >> b & 1) r |= 1ull << (w - 1 - b);
                        break;
                    }
                    case 1: {  // REV16
                        r = ((a & 0x00ff00ff00ff00ffull) << 8) | ((a >> 8) & 0x00ff00ff00ff00ffull);
                        break;
                    }
                    case 2:  // REV32 (64-bit), REV (32-bit)
                        r = sf ? ((uint64_t)__builtin_bswap32((uint32_t)a) | (uint64_t)__builtin_bswap32((uint32_t)(a >> 32)) << 32)
                               : __builtin_bswap32((uint32_t)a);
                        break;
                    case 3:
                        if (!sf) { undefined(c, i, pc); return; }
                        r = __builtin_bswap64(a);
                        break;
                    case 4: r = a ? (sf ? __builtin_clzll(a) : __builtin_clz((uint32_t)a)) : w; break;  // CLZ
                    case 5: {  // CLS
                        const uint64_t x = ((a ^ (a << 1)) & ones(w)) | 1;  // bit k: a<k> != a<k-1>
                        r = sf ? __builtin_clzll(x) : __builtin_clz((uint32_t)x);
                        break;
                    }
                    default: undefined(c, i, pc); return;
                }
                setX(c, rd, r, sf);
                return;
            }
            // data processing (2 source)
            const uint64_t a = X(c, rn), b = X(c, rm);
            uint64_t r;
            switch (opc) {
                case 2:  // UDIV
                    if (sf) r = b ? a / b : 0;
                    else r = (uint32_t)b ? (uint32_t)a / (uint32_t)b : 0;
                    break;
                case 3:  // SDIV
                    if (sf) {
                        const int64_t x = (int64_t)a, y = (int64_t)b;
                        r = !y ? 0 : (x == INT64_MIN && y == -1) ? (uint64_t)x : (uint64_t)(x / y);
                    } else {
                        const int32_t x = (int32_t)a, y = (int32_t)b;
                        r = !y ? 0 : (x == INT32_MIN && y == -1) ? (uint32_t)x : (uint32_t)(x / y);
                    }
                    break;
                case 8: case 9: case 10: case 11:  // LSLV, LSRV, ASRV, RORV
                    r = shiftReg(a, opc - 8, (unsigned)(b & (w - 1)), sf);
                    break;
                default: undefined(c, i, pc); return;
            }
            setX(c, rd, r, sf);
            return;
        }
        default:
            if (op2 & 8) {  // data processing (3 source)
                const unsigned op31 = (i >> 21) & 7, o0 = i >> 15 & 1, ra = (i >> 10) & 31;
                if ((i >> 29) & 3) break;
                const uint64_t a = X(c, rn), b = X(c, rm), acc = X(c, ra);
                uint64_t r;
                switch (op31 << 1 | o0) {
                    case 0: r = acc + a * b; break;                                                            // MADD
                    case 1: r = acc - a * b; break;                                                            // MSUB
                    case 2: if (!sf) { undefined(c, i, pc); return; } r = acc + (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b); break;  // SMADDL
                    case 3: if (!sf) { undefined(c, i, pc); return; } r = acc - (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b); break;  // SMSUBL
                    case 4: if (!sf) { undefined(c, i, pc); return; } r = (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64); break;       // SMULH
                    case 10: if (!sf) { undefined(c, i, pc); return; } r = acc + (uint64_t)(uint32_t)a * (uint32_t)b; break;                  // UMADDL
                    case 11: if (!sf) { undefined(c, i, pc); return; } r = acc - (uint64_t)(uint32_t)a * (uint32_t)b; break;                  // UMSUBL
                    case 12: if (!sf) { undefined(c, i, pc); return; } r = (uint64_t)(((unsigned __int128)a * b) >> 64); break;               // UMULH
                    default: undefined(c, i, pc); return;
                }
                setX(c, rd, r, sf);
                return;
            }
            break;
    }
    undefined(c, i, pc);
}

}  // namespace a64
}  // namespace vp

#include "a64ldst.inc"
#include "a64sys.inc"
#include "a64simd.inc"
