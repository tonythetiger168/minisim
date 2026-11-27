// a64sys.inc - A64 system instructions: hints, barriers, MSR (immediate),
// MRS / MSR with the system registers of EL0 and EL1 (and the read-only
// identification registers), SYS (DC, IC, TLBI, AT), and the firmware
// behind HVC / SMC (PSCI 1.1, SMCCC 1.1). Part of a64machine.hpp.
#pragma once

namespace vp {
namespace a64 {

inline void Machine::system(Cpu& c, uint32_t i, uint64_t pc) {
    const unsigned l = i >> 21 & 1, op0 = (i >> 19) & 3, op1 = (i >> 16) & 7, crn = (i >> 12) & 15, crm = (i >> 8) & 15,
                   op2 = (i >> 5) & 7, rt = i & 31;
    if (op0 == 0 && !l) {
        if (crn == 2) {  // hints
            if (rt != 31) { undefined(c, i, pc); return; }
            switch (crm << 3 | op2) {
                case 2:  // WFE: sleeps only until the next event
                    if (c.eventReg) c.eventReg = false;
                    return;
                case 3:  // WFI
                    if (!c.irqLine) c.wfi = true;
                    return;
                case 4: case 5:  // SEV, SEVL
                    for (auto& o : cpus) o->eventReg = true;
                    return;
                default: return;  // NOP, YIELD, ESB, PSB, BTI, PAC hints (no PAC): no effect
            }
        }
        if (crn == 3) {  // barriers, CLREX
            if (rt != 31) { undefined(c, i, pc); return; }
            if (op2 == 2) { clearMonitor(c); return; }
            if (op2 == 4 && c.tlbiPending) waitFlushes(c);  // DSB: TLBIs on other processors done
            if (op2 == 4 || op2 == 5 || op2 == 6) return;  // DSB, DMB, ISB
            undefined(c, i, pc);
            return;
        }
        if (crn == 4 && rt == 31) {  // MSR (immediate)
            if (c.el == 0 && !(op1 == 3 && (op2 == 6 || op2 == 7) && (c.sctlr >> 9 & 1))) { undefined(c, i, pc); return; }
            switch (op1 << 3 | op2) {
                case 0x05: c.spsel = crm & 1; return;                        // SPSel
                case 0x1e: c.daif |= (crm & 15) << 6; return;                // DAIFSet
                case 0x1f: c.daif &= ~((crm & 15) << 6); return;             // DAIFClr
                default: undefined(c, i, pc); return;                        // PAN, UAO, SSBS, DIT: not there
            }
        }
        undefined(c, i, pc);
        return;
    }
    if (op0 == 1) { sysOp(c, i, pc); return; }
    if (op0 >= 2) {  // MRS, MSR (register)
        const uint32_t reg = SR(op0, op1, crn, crm, op2);
        uint64_t v = l ? 0 : X(c, rt);
        if (!sysReg(c, reg, !l, v, i, pc)) return;
        if (l) setX(c, rt, v, true);
        return;
    }
    undefined(c, i, pc);
}

// The ESR_EL1 syndrome of a trapped MSR / MRS / SYS (EC 0x18).
inline uint32_t sysIss(uint32_t i) {
    const unsigned l = i >> 21 & 1, op0 = (i >> 19) & 3, op1 = (i >> 16) & 7, crn = (i >> 12) & 15, crm = (i >> 8) & 15,
                   op2 = (i >> 5) & 7, rt = i & 31;
    return op0 << 20 | op2 << 17 | op1 << 14 | crn << 10 | rt << 5 | crm << 1 | l;
}

// A system register access. False when it raised an exception.
inline bool Machine::sysReg(Cpu& c, uint32_t reg, bool write, uint64_t& v, uint32_t i, uint64_t pc) {
    const unsigned op0 = reg >> 14, op1 = (reg >> 11) & 7, crn = (reg >> 7) & 15, crm = (reg >> 3) & 15;
    auto ro = [&](uint64_t val) { if (write) return false; v = val; return true; };
    auto rw = [&](uint64_t& r, uint64_t mask = ~0ull) { if (write) r = v & mask; else v = r; return true; };
    auto rw32 = [&](uint32_t& r, uint32_t mask = ~0u) { if (write) r = (uint32_t)v & mask; else v = r; return true; };
    // EL0 may reach only some (op1 = 3), and some only when EL1 allows it
    if (c.el == 0) {
        bool ok = false;
        switch (reg) {
            case SR(3, 3, 4, 2, 0): case SR(3, 3, 4, 4, 0): case SR(3, 3, 4, 4, 1): case SR(3, 3, 13, 0, 2):
                ok = true; break;                                                  // NZCV, FPCR, FPSR, TPIDR_EL0
            case SR(3, 3, 13, 0, 3): ok = !write; break;                           // TPIDRRO_EL0
            case SR(3, 3, 4, 2, 1): ok = c.sctlr >> 9 & 1; break;                  // DAIF (UMA)
            case SR(3, 3, 0, 0, 1): ok = !write && (c.sctlr >> 15 & 1); break;     // CTR_EL0 (UCT)
            case SR(3, 3, 0, 0, 7): ok = !write; break;                            // DCZID_EL0
            case SR(3, 3, 14, 0, 0): ok = !write && (c.cntkctl & 3); break;        // CNTFRQ_EL0
            case SR(3, 3, 14, 0, 1): ok = !write && (c.cntkctl & 1); break;        // CNTPCT_EL0
            case SR(3, 3, 14, 0, 2): ok = !write && (c.cntkctl & 2); break;        // CNTVCT_EL0
            default: break;
        }
        if (!ok) {
            // trapped to EL1 (EC 0x18) where EL1 could have allowed it,
            // undefined otherwise
            if (op0 == 3 && op1 == 3) exception(c, EC_SYS, sysIss(i), pc);
            else undefined(c, i, pc);
            return false;
        }
    }
    if (op0 == 3 && op1 == 0 && crn == 12 && (crm >= 8 && crm <= 12)) {  // the GIC CPU interface
        DevLock l(*this);
        if (gic && gic->icc(c, reg, write, v)) return true;
    }
    if (reg == SR(3, 0, 4, 6, 0)) { DevLock l(*this); if (gic && gic->icc(c, reg, write, v)) return true; }  // ICC_PMR_EL1
    bool ok = true;
    switch (reg) {
        // identification
        case SR(3, 0, 0, 0, 0): ok = ro(0x000f0510); break;            // MIDR_EL1: implementer 0, architecture 0xf
        case SR(3, 0, 0, 0, 5): ok = ro(c.mpidr); break;               // MPIDR_EL1
        case SR(3, 0, 0, 0, 6): ok = ro(0); break;                     // REVIDR_EL1
        case SR(3, 0, 0, 4, 0): ok = ro(1ull << 60 | 1ull << 56 | 0x1000011); break;  // ID_AA64PFR0_EL1: CSV3, CSV2 (no speculation); EL0, EL1 AArch64; FP, AdvSIMD; GIC sysregs
        case SR(3, 0, 0, 6, 0): ok = ro(2u << 20 | 1u << 16); break;  // ID_AA64ISAR0_EL1: LSE atomics, CRC32
        case SR(3, 0, 0, 5, 0): ok = ro(0x6); break;                   // ID_AA64DFR0_EL1: Armv8.0 debug, no PMU
        case SR(3, 0, 0, 7, 0): ok = ro(0xf0 << 20 | 0x2 << 4 | 0x2); break;  // ID_AA64MMFR0_EL1: 40-bit PA, 16-bit ASID, 4K only
        case SR(3, 3, 0, 0, 1): ok = ro(1ull << 31 | 1ull << 29 | 1ull << 28 | 4u << 24 | 4u << 20 | 4u << 16 | 3u << 14 | 4u); break;  // CTR_EL0: DIC, IDC, 64-byte lines
        case SR(3, 3, 0, 0, 7): ok = ro(4); break;                     // DCZID_EL0: 64 bytes
        case SR(3, 1, 0, 0, 1): ok = ro(0); break;                     // CLIDR_EL1: no caches to manage
        case SR(3, 1, 0, 0, 0): ok = ro(0); break;                     // CCSIDR_EL1
        case SR(3, 1, 0, 0, 7): ok = ro(0); break;                     // AIDR_EL1
        case SR(3, 2, 0, 0, 0): ok = rw(c.csselr, 0xf); break;         // CSSELR_EL1
        // PSTATE
        case SR(3, 3, 4, 2, 0): if (write) c.nzcv = (uint32_t)v & 0xf0000000u; else v = c.nzcv; break;  // NZCV
        case SR(3, 3, 4, 2, 1): if (write) c.daif = (uint32_t)v & 0x3c0; else v = c.daif; break;          // DAIF
        case SR(3, 0, 4, 2, 2): ok = ro((uint64_t)c.el << 2); break;  // CurrentEL
        case SR(3, 0, 4, 2, 0): if (write) c.spsel = v & 1; else v = c.spsel; break;  // SPSel
        case SR(3, 3, 4, 4, 0): ok = rw32(c.fpcr, 0x07c00000); break;  // FPCR: AHP, DN, FZ, RMode (no traps)
        case SR(3, 3, 4, 4, 1): ok = rw32(c.fpsr, 0xf800009f); break;  // FPSR
        // EL1 state
        case SR(3, 0, 1, 0, 0):  // SCTLR_EL1
            if (write) {
                if ((c.sctlr ^ v) & 1) c.flushTlb();
                c.flushFast();
                c.sctlr = v;
            } else v = c.sctlr;
            break;
        case SR(3, 0, 1, 0, 1): ok = rw(c.actlr); break;
        case SR(3, 0, 1, 0, 2): ok = rw(c.cpacr, 3u << 20); break;     // CPACR_EL1: FPEN only
        case SR(3, 0, 2, 0, 0): ok = rw(c.ttbr0); if (write) c.flushFast(); break;
        case SR(3, 0, 2, 0, 1): ok = rw(c.ttbr1); if (write) c.flushFast(); break;
        case SR(3, 0, 2, 0, 2): if (write) { c.tcr = v; c.flushTlb(); } else v = c.tcr; break;
        case SR(3, 0, 4, 0, 0): ok = rw(c.spsr); break;
        case SR(3, 0, 4, 0, 1): ok = rw(c.elr); break;
        case SR(3, 0, 4, 1, 0): ok = rw(c.sp[0]); break;               // SP_EL0
        case SR(3, 0, 5, 1, 0): ok = rw(c.afsr0); break;
        case SR(3, 0, 5, 1, 1): ok = rw(c.afsr1); break;
        case SR(3, 0, 5, 2, 0): ok = rw(c.esr); break;
        case SR(3, 0, 5, 3, 0): if (!write) v = 0; break;              // ERRIDR_EL1: no RAS records
        case SR(3, 0, 6, 0, 0): ok = rw(c.far); break;
        case SR(3, 0, 7, 4, 0): ok = rw(c.par); break;
        case SR(3, 0, 10, 2, 0): ok = rw(c.mair); break;
        case SR(3, 0, 10, 3, 0): ok = rw(c.amair); break;
        case SR(3, 0, 12, 0, 0): ok = rw(c.vbar, ~0x7ffull); break;
        case SR(3, 0, 12, 1, 1): ok = ro(0); break;                    // DISR_EL1
        case SR(3, 0, 13, 0, 1): ok = rw(c.contextidr, 0xffffffff); break;
        case SR(3, 0, 13, 0, 4): ok = rw(c.tpidr1); break;
        case SR(3, 3, 13, 0, 2): ok = rw(c.tpidr0); break;
        case SR(3, 3, 13, 0, 3): ok = rw(c.tpidrro0); break;
        case SR(3, 0, 14, 1, 0): ok = rw(c.cntkctl, 0x3ff); break;
        // the generic timer
        case SR(3, 3, 14, 0, 0): ok = ro(timerHz); break;              // CNTFRQ_EL0
        case SR(3, 3, 14, 0, 1): ok = ro(counterRead(c)); break;       // CNTPCT_EL0
        case SR(3, 3, 14, 0, 2): ok = ro(counterRead(c) - c.cntvoff); break;  // CNTVCT_EL0
        case SR(3, 3, 14, 0, 5): ok = ro(counterRead(c)); break;       // CNTPCTSS_EL0
        case SR(3, 3, 14, 0, 6): ok = ro(counterRead(c) - c.cntvoff); break;  // CNTVCTSS_EL0
        case SR(3, 3, 14, 2, 1): case SR(3, 3, 14, 3, 1): {           // CNTP_CTL_EL0, CNTV_CTL_EL0
            const bool virt = crm == 3;
            uint32_t& ctl = virt ? c.cntvCtl : c.cntpCtl;
            if (write) ctl = (uint32_t)v & 3;
            else v = ctl | (timerFires(c, virt) ? 4u : 0);
            updateTimers(c);
            break;
        }
        case SR(3, 3, 14, 2, 2): case SR(3, 3, 14, 3, 2): {           // CNTP_CVAL_EL0, CNTV_CVAL_EL0
            uint64_t& cv = crm == 3 ? c.cntvCval : c.cntpCval;
            if (write) cv = v; else v = cv;
            updateTimers(c);
            break;
        }
        case SR(3, 3, 14, 2, 0): case SR(3, 3, 14, 3, 0): {           // CNTP_TVAL_EL0, CNTV_TVAL_EL0
            const bool virt = crm == 3;
            const uint64_t cnt = counter(c) - (virt ? c.cntvoff : 0);
            uint64_t& cv = virt ? c.cntvCval : c.cntpCval;
            if (write) cv = cnt + (uint64_t)(int64_t)(int32_t)v;
            else v = (uint32_t)(cv - cnt);
            updateTimers(c);
            break;
        }
        // debug: the OS lock and the monitor's controls, kept and ignored
        case SR(2, 0, 0, 2, 2): ok = rw(c.mdscr); break;               // MDSCR_EL1
        case SR(2, 0, 0, 2, 0): ok = rw(c.mdccint); break;             // MDCCINT_EL1
        case SR(2, 0, 1, 0, 4): if (write) c.oslsr = (v & 1) << 1; else ok = false; break;  // OSLAR_EL1
        case SR(2, 0, 1, 1, 4): ok = ro(c.oslsr | 8); break;           // OSLSR_EL1
        case SR(2, 0, 1, 3, 4): if (!write) v = 0; break;              // OSDLR_EL1
        case SR(2, 0, 0, 0, 2): case SR(2, 0, 0, 3, 2): if (!write) v = 0; break;  // OSDTRRX/TX
        case SR(2, 3, 0, 1, 0): ok = ro(0); break;                     // MDCCSR_EL0
        default:
            // the rest of the identification space (op0 3, op1 0, CRn 0)
            // reads as zero: the feature is not there
            if (op0 == 3 && op1 == 0 && crn == 0 && crm >= 1) { ok = ro(0); break; }
            // breakpoint and watchpoint registers (none implemented, but
            // BRPs and WRPs say one of each): kept as zero
            if (op0 == 2 && op1 == 0 && crn == 0 && crm <= 15 && ((reg & 7) >= 4)) { if (!write) v = 0; break; }
            ok = false;
            break;
    }
    if (!ok) { undefined(c, i, pc); return false; }
    return true;
}

// SYS: DC, IC, AT, TLBI (C5.3)
inline void Machine::sysOp(Cpu& c, uint32_t i, uint64_t pc) {
    const unsigned l = i >> 21 & 1, op1 = (i >> 16) & 7, crn = (i >> 12) & 15, crm = (i >> 8) & 15, op2 = (i >> 5) & 7,
                   rt = i & 31;
    if (l) { undefined(c, i, pc); return; }  // SYSL
    const uint64_t v = X(c, rt);
    if (crn == 7) {
        if (op1 == 3 && crm == 4 && op2 == 1) {  // DC ZVA
            if (c.el == 0 && !(c.sctlr >> 14 & 1)) { exception(c, EC_SYS, sysIss(i), pc); return; }
            const uint64_t a = v & ~63ull;
            uint64_t pa;
            uint8_t* h;
            if (!xlate(c, a, 1, false, pa, h)) return;
            if (monitors_) clearMonitors(c, pa, 64);
            if (h) memset(h + (pa & 0xfff), 0, 64);
            else for (unsigned k = 0; k < 64; k += 8) if (!wr(c, a + k, 8, 0)) return;
            return;
        }
        if (crm == 8 || crm == 9) {  // AT S1E1R/W, S1E0R/W: translate, the result in PAR_EL1
            if (c.el == 0 || op1 != 0) { undefined(c, i, pc); return; }
            const bool w = op2 & 1, e0 = crm == 8 && (op2 & 2);
            uint64_t pa;
            uint8_t* h;
            uint32_t fsc = 0;
            if (xlate(c, v, w ? 1 : 0, e0, pa, h, false, &fsc)) c.par = pa & 0x0000fffffffff000ull;
            else c.par = 1 | fsc << 1;
            return;
        }
        // cache maintenance: nothing to keep coherent (CTR_EL0 says DIC and IDC)
        if (c.el == 0) {
            const bool user = op1 == 3 && ((crm == 5 && op2 == 1) || ((crm == 11 || crm == 14 || crm == 10 || crm == 12 || crm == 13) && op2 == 1));
            if (!user) { undefined(c, i, pc); return; }
            if (!(c.sctlr >> 26 & 1)) { exception(c, EC_SYS, sysIss(i), pc); return; }  // UCI
        }
        return;
    }
    if (crn == 8 && op1 == 0) {  // TLBI
        if (c.el == 0) { undefined(c, i, pc); return; }
        // invalidate everywhere (the inner-shareable forms are the same for
        // processors that take turns)
        const bool va = op2 == 1 || op2 == 3 || op2 == 5 || op2 == 7;
        const bool asidOnly = op2 == 2;
        for (auto& o : cpus) {
            if (!(crm == 3 || o.get() == &c)) continue;  // CRm 3: inner shareable
            if (mt && o.get() != &c) {
                // -mt: it is running; it empties its whole TLB before its
                // next instruction, and DSB waits for that
                o->flushReq.store(1, std::memory_order_seq_cst);
                c.tlbiPending = true;
                continue;
            }
            o->flushFast();
            if (va) {
                const uint64_t page = (v & 0xfffffffffffull);
                for (auto& e : o->tlb) {
                    if (e.tag == ~0ull) continue;
                    if ((e.tag & 0xfffffffffffull) == page) e.tag = ~0ull;
                }
            } else if (asidOnly) {
                const uint16_t asid = (uint16_t)(v >> 48);
                for (auto& e : o->tlb) if (!e.global && e.asid == asid) e.tag = ~0ull;
            } else o->flushTlb();
        }
        if (crm != 3 && crm != 7) return;
        return;
    }
    undefined(c, i, pc);
}

// HVC or SMC from EL1: the firmware's functions (PSCI 1.1, SMCCC 1.1).
// False: not a call it knows (the instruction is then undefined).
inline bool Machine::firmware(Cpu& c) {
    DevLock l(*this);
    const uint32_t fn = (uint32_t)c.x[0];
    const int64_t NOT_SUPPORTED = -1, INVALID = -2, ALREADY_ON = -4, SUCCESS = 0;
    int64_t r = NOT_SUPPORTED;
    switch (fn) {
        case 0x80000000: r = 0x10001; break;                 // SMCCC_VERSION 1.1
        case 0x80000001: r = NOT_SUPPORTED; break;           // SMCCC_ARCH_FEATURES
        case 0x84000000: r = 0x10001; break;                 // PSCI_VERSION 1.1
        case 0x84000006: r = 2; break;                       // MIGRATE_INFO_TYPE: not needed
        case 0x8400000a: {                                   // PSCI_FEATURES
            const uint32_t q = (uint32_t)c.x[1];
            switch (q) {
                case 0x84000000: case 0x84000002: case 0x84000008: case 0x84000009: case 0x8400000a:
                case 0x84000003: case 0xc4000003: case 0x84000004: case 0xc4000004: case 0x84000006:
                case 0x80000000: r = 0; break;
                default: r = NOT_SUPPORTED; break;
            }
            break;
        }
        case 0x84000002:  // CPU_OFF
            c.off = true;
            c.wfi = false;
            r = SUCCESS;
            break;
        case 0x84000003: case 0xc4000003: {  // CPU_ON
            const uint64_t target = c.x[1] & 0xff00ffffffull, entry = c.x[2], ctx = c.x[3];
            r = INVALID;
            for (auto& o : cpus) {
                if ((o->mpidr & 0xff00ffffffull) != target) continue;
                if (!o->off) { r = ALREADY_ON; break; }
                o->off = false;
                o->wfi = false;
                o->el = 1;
                o->spsel = true;
                o->daif = 0x3c0;
                o->nzcv = 0;
                o->sctlr = 0x30d00800;
                o->pc = entry;
                o->x[0] = ctx;
                o->cycle = std::max(o->cycle, c.cycle);
                o->flushTlb();
                r = SUCCESS;
                break;
            }
            break;
        }
        case 0x84000004: case 0xc4000004: {  // AFFINITY_INFO: 0 on, 1 off
            r = INVALID;
            for (auto& o : cpus)
                if ((o->mpidr & 0xff00ffffffull) == (c.x[1] & 0xff00ffffffull)) { r = o->off ? 1 : 0; break; }
            break;
        }
        case 0x84000008: requestExit(0); r = SUCCESS; break;   // SYSTEM_OFF
        case 0x84000009: requestExit(0); r = SUCCESS; break;   // SYSTEM_RESET: the run ends
        default:
            if ((fn >> 24) == 0x84 || (fn >> 24) == 0xc4 || (fn >> 24) == 0x80 || (fn >> 24) == 0x86 || (fn >> 24) == 0xc6) r = NOT_SUPPORTED;
            else r = NOT_SUPPORTED;
            break;
    }
    c.x[0] = (uint64_t)r;
    return true;
}

}  // namespace a64
}  // namespace vp
