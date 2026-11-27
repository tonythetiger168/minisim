// a64ldst.inc - A64 loads and stores (C4.1.88): exclusives and
// load-acquire / store-release, literal loads, pairs, the immediate,
// unscaled, unprivileged and register-offset forms, for the general and the
// SIMD and floating-point registers. Part of a64machine.hpp.
#pragma once

namespace vp {
namespace a64 {

inline void Machine::loadStore(Cpu& c, uint32_t i, uint64_t pc) {
    const unsigned rt = i & 31, rn = (i >> 5) & 31;
    const bool vec = i >> 26 & 1;
    if (vec && !fpAllowed(c, pc)) return;
    auto spCheck = [&](uint64_t base) {  // SP alignment (SCTLR_EL1.SA / SA0)
        if (rn == 31 && (base & 15) && (c.sctlr >> (c.el ? 3 : 4) & 1)) {
            exception(c, EC_SP_ALIGN, 0, pc);
            return false;
        }
        return true;
    };
    // a SIMD and floating-point register of 1 to 16 bytes
    auto vload = [&](uint64_t a, unsigned n, u128& v) {
        uint64_t lo = 0, hi = 0;
        if (n == 16) {
            if (!rd(c, a, 8, lo) || !rd(c, a + 8, 8, hi)) return false;
        } else if (!rd(c, a, n, lo)) return false;
        v = (u128)hi << 64 | lo;
        return true;
    };
    auto vstore = [&](uint64_t a, unsigned n, u128 v) {
        if (n == 16) {
            // both halves translated first
            uint64_t pa;
            uint8_t* h;
            if (!xlate(c, a, 1, false, pa, h) || !xlate(c, a + 15, 1, false, pa, h)) return false;
            return wr(c, a, 8, (uint64_t)v) && wr(c, a + 8, 8, (uint64_t)(v >> 64));
        }
        return wr(c, a, n, (uint64_t)v & ones(8 * n));
    };

    // Load/store exclusive and ordered (size 31:30, 001000)
    if ((i & 0x3f000000) == 0x08000000) {
        const unsigned size = i >> 30, o2 = i >> 23 & 1, l = i >> 22 & 1, o1 = i >> 21 & 1, rs = (i >> 16) & 31, rt2 = (i >> 10) & 31;
        const unsigned n = 1u << size;
        if (vec) { undefined(c, i, pc); return; }
        const uint64_t a = XS(c, rn);
        if ((o2 && o1) || (!o2 && o1 && size < 2)) {  // CAS, CASP (Armv8.1 LSE)
            if (rt2 != 31) { undefined(c, i, pc); return; }
            const bool pairCas = !o2;
            const unsigned en = pairCas ? 4u << (size & 1) : n;  // bytes per element
            if (pairCas && ((rs & 1) || (rt & 1))) { undefined(c, i, pc); return; }
            if (!spCheck(a)) return;
            const unsigned total = pairCas ? 2 * en : en;
            if (a & (total - 1)) { c.far = a; exception(c, c.el ? EC_DABT_CUR : EC_DABT_LOW, F_ALIGN | 1u << 6, pc); return; }
            uint64_t pa;
            uint8_t* h;
            if (!xlate(c, a, 1, false, pa, h)) return;  // a write, compared equal or not
            const uint64_t m = ones(8 * en);
            const bool x64 = en == 8;
            if (!pairCas) {
                uint64_t old;
                if (!rd(c, a, en, old)) return;
                if (old == (X(c, rs) & m) && !wr(c, a, en, X(c, rt) & m)) return;
                setX(c, rs, old, x64);
                return;
            }
            uint64_t o1v, o2v;
            if (!rd(c, a, en, o1v) || !rd(c, a + en, en, o2v)) return;
            if (o1v == (X(c, rs) & m) && o2v == (X(c, rs + 1) & m)) {
                if (!wr(c, a, en, X(c, rt) & m) || !wr(c, a + en, en, X(c, rt + 1) & m)) return;
            }
            setX(c, rs, o1v, x64);
            setX(c, rs + 1, o2v, x64);
            return;
        }
        if (!spCheck(a)) return;
        if (o2) {  // LDAR, STLR (o0 = 1), LDLAR, STLLR (o0 = 0)
            if (a & (n - 1)) { c.far = a; exception(c, c.el ? EC_DABT_CUR : EC_DABT_LOW, F_ALIGN | (l ? 0 : 1u << 6), pc); return; }
            if (l) { uint64_t v; if (rd(c, a, n, v)) setX(c, rt, v, true); }
            else wr(c, a, n, X(c, rt));
            return;
        }
        const bool pair = o1;
        if (pair && size < 2) { undefined(c, i, pc); return; }
        const unsigned total = pair ? 2 * n : n;
        if (a & (total - 1)) { c.far = a; exception(c, c.el ? EC_DABT_CUR : EC_DABT_LOW, F_ALIGN | (l ? 0 : 1u << 6), pc); return; }
        if (l) {  // LDXR, LDAXR, LDXP, LDAXP
            uint64_t v1 = 0, v2 = 0;
            if (!rd(c, a, n, v1)) return;
            if (pair && !rd(c, a + n, n, v2)) return;
            uint64_t pa;
            uint8_t* h;
            xlate(c, a, 0, false, pa, h);
            setMonitor(c, pa, total);
            setX(c, rt, v1, true);
            if (pair) setX(c, rt2, v2, true);
            return;
        }
        // STXR, STLXR, STXP, STLXP: Rs gets 0 on success
        uint64_t pa;
        uint8_t* h;
        if (!xlate(c, a, 1, false, pa, h)) return;
        bool ok = c.exAddr == pa && c.exSize == total;
        if (ok) {
            if (pair) {
                if (n == 8) { if (!wr(c, a, 8, X(c, rt)) || !wr(c, a + 8, 8, X(c, rt2))) return; }
                else if (!wr(c, a, 8, (X(c, rt) & 0xffffffffull) | X(c, rt2) << 32)) return;
            } else if (!wr(c, a, n, X(c, rt))) return;
        }
        clearMonitor(c);
        setX(c, rs, ok ? 0 : 1, false);
        return;
    }
    // Load register (literal)
    if ((i & 0x3b000000) == 0x18000000) {
        const unsigned opc = i >> 30;
        const int64_t off = (int64_t)((uint64_t)((i >> 5) & 0x7ffff) << 45) >> 43;
        const uint64_t a = pc + (uint64_t)off;
        if (vec) {
            if (opc == 3) { undefined(c, i, pc); return; }
            u128 v;
            if (vload(a, 4u << opc, v)) c.v[rt] = v;
            return;
        }
        if (opc == 3) return;  // PRFM
        uint64_t v;
        if (!rd(c, a, opc == 0 ? 4 : opc == 1 ? 8 : 4, v)) return;
        if (opc == 2) v = (uint64_t)(int64_t)(int32_t)v;
        setX(c, rt, v, true);
        return;
    }
    // Load/store pair
    if ((i & 0x38000000) == 0x28000000) {
        const unsigned opc = i >> 30, mode = (i >> 23) & 3, l = i >> 22 & 1, rt2 = (i >> 10) & 31;
        const int64_t imm7 = (int64_t)((uint64_t)((i >> 15) & 0x7f) << 57) >> 57;
        unsigned n;
        bool sgn = false;
        if (vec) {
            if (opc == 3) { undefined(c, i, pc); return; }
            n = 4u << opc;
        } else {
            if (opc == 3 || (opc == 1 && !l)) { undefined(c, i, pc); return; }  // (STGP)
            if (opc == 1) sgn = true;  // LDPSW
            n = opc == 2 ? 8 : 4;
        }
        const uint64_t base = XS(c, rn);
        if (!spCheck(base)) return;
        const uint64_t off = (uint64_t)(imm7 * (int64_t)n);
        const uint64_t a = mode == 1 ? base : base + off;
        if (l) {
            if (vec) {
                u128 v1, v2;
                if (!vload(a, n, v1) || !vload(a + n, n, v2)) return;
                c.v[rt] = v1;
                c.v[rt2] = v2;
            } else {
                uint64_t v1, v2;
                if (!rd(c, a, n, v1) || !rd(c, a + n, n, v2)) return;
                if (sgn) { v1 = (uint64_t)(int64_t)(int32_t)v1; v2 = (uint64_t)(int64_t)(int32_t)v2; }
                setX(c, rt, v1, true);
                setX(c, rt2, v2, true);
            }
        } else {
            if (vec) {
                if (!vstore(a, n, c.v[rt]) || !vstore(a + n, n, c.v[rt2])) return;
            } else {
                // both translated before either is written
                uint64_t pa;
                uint8_t* h;
                if (!xlate(c, a, 1, false, pa, h) || !xlate(c, a + 2 * n - 1, 1, false, pa, h)) return;
                const uint64_t v1 = X(c, rt), v2 = X(c, rt2);
                if (!wr(c, a, n, v1) || !wr(c, a + n, n, v2)) return;
            }
        }
        if (mode == 1 || mode == 3) setXS(c, rn, base + off, true);
        return;
    }
    // Load/store register: immediate, register offset, unsigned immediate
    if ((i & 0x38000000) == 0x38000000) {
        if (!vec && !(i >> 24 & 1) && (i >> 21 & 1) && !((i >> 10) & 3)) { atomic(c, i, pc); return; }
        const unsigned size = i >> 30, opc = (i >> 22) & 3;
        unsigned n, scale;
        bool load, sgn = false, to32 = false;
        if (vec) {
            if (opc & 2) {
                if (size) { undefined(c, i, pc); return; }
                n = 16;
                scale = 4;
                load = opc & 1;
            } else {
                n = 1u << size;
                scale = size;
                load = opc & 1;
            }
        } else {
            n = 1u << size;
            scale = size;
            if (opc == 0) load = false;
            else if (opc == 1) load = true;
            else if (size == 3) {
                if (opc == 2) load = true, n = 0;  // PRFM
                else { undefined(c, i, pc); return; }
            } else if (size == 2 && opc == 3) { undefined(c, i, pc); return; }
            else { load = true; sgn = true; to32 = opc == 3; }
        }
        uint64_t base = XS(c, rn), a;
        int wb = 0;  // 1 post-index, 2 pre-index
        bool unpriv = false;
        if (i >> 24 & 1) {  // unsigned immediate
            a = base + ((uint64_t)((i >> 10) & 0xfff) << scale);
        } else if (i >> 21 & 1) {
            if (((i >> 10) & 3) != 2) { undefined(c, i, pc); return; }  // PAC loads: not there
            const unsigned opt = (i >> 13) & 7;
            if (!(opt & 2)) { undefined(c, i, pc); return; }
            a = base + extendReg(X(c, (i >> 16) & 31), opt, (i >> 12 & 1) ? scale : 0);
        } else {
            const int64_t imm9 = (int64_t)((uint64_t)((i >> 12) & 0x1ff) << 55) >> 55;
            switch ((i >> 10) & 3) {
                case 0: a = base + (uint64_t)imm9; break;                 // unscaled
                case 1: a = base; wb = 1; break;                          // post-index
                case 2: a = base + (uint64_t)imm9; unpriv = c.el == 1; if (vec) { undefined(c, i, pc); return; } break;
                default: a = base + (uint64_t)imm9; wb = 2; break;        // pre-index
            }
            if (wb) base += (uint64_t)imm9;
        }
        if (!spCheck(XS(c, rn))) return;
        if (n == 0) return;  // PRFM
        if (vec) {
            if (load) {
                u128 v;
                if (!vload(a, n, v)) return;
                c.v[rt] = v;
            } else if (!vstore(a, n, c.v[rt])) return;
        } else if (load) {
            uint64_t v;
            if (!rd(c, a, n, v, unpriv)) return;
            if (sgn) {
                v = n == 1 ? (uint64_t)(int64_t)(int8_t)v : n == 2 ? (uint64_t)(int64_t)(int16_t)v : (uint64_t)(int64_t)(int32_t)v;
                if (to32) v = (uint32_t)v;
            }
            if (wb) setXS(c, rn, base, true);
            setX(c, rt, v, true);
            return;
        } else {
            if (!wr(c, a, n, X(c, rt), unpriv)) return;
        }
        if (wb) setXS(c, rn, base, true);
        return;
    }
    // AdvSIMD load/store multiple structures and single structure
    if ((i & 0xbf000000) == 0x0c000000 || (i & 0xbf000000) == 0x0d000000) {
        simdLdSt(c, i, pc);
        return;
    }
    undefined(c, i, pc);
}

// Atomic memory operations (Armv8.1 LSE): LDADD, LDCLR, LDEOR, LDSET,
// LDSMAX, LDSMIN, LDUMAX, LDUMIN and SWP, with their acquire / release
// forms (the processors take turns, so ordering needs nothing more).
inline void Machine::atomic(Cpu& c, uint32_t i, uint64_t pc) {
    const unsigned size = i >> 30, rs = (i >> 16) & 31, rn = (i >> 5) & 31, rt = i & 31;
    const unsigned o3 = i >> 15 & 1, opc = (i >> 12) & 7, n = 1u << size;
    if (o3 && opc) { undefined(c, i, pc); return; }  // LDAPR (Armv8.3): not there
    const uint64_t a = XS(c, rn);
    if (rn == 31 && (a & 15) && (c.sctlr >> (c.el ? 3 : 4) & 1)) { exception(c, EC_SP_ALIGN, 0, pc); return; }
    if (a & (n - 1)) { c.far = a; exception(c, c.el ? EC_DABT_CUR : EC_DABT_LOW, F_ALIGN | 1u << 6, pc); return; }
    uint64_t pa;
    uint8_t* h;
    if (!xlate(c, a, 1, false, pa, h)) return;
    uint64_t old;
    if (!rd(c, a, n, old)) return;
    const uint64_t m = ones(8 * n), s = X(c, rs) & m;
    auto sgn = [&](uint64_t v) { return (int64_t)(v << (64 - 8 * n)) >> (64 - 8 * n); };
    uint64_t r;
    if (o3) r = s;  // SWP
    else switch (opc) {
        case 0: r = old + s; break;
        case 1: r = old & ~s; break;
        case 2: r = old ^ s; break;
        case 3: r = old | s; break;
        case 4: r = sgn(old) > sgn(s) ? old : s; break;
        case 5: r = sgn(old) < sgn(s) ? old : s; break;
        case 6: r = old > s ? old : s; break;
        default: r = old < s ? old : s; break;
    }
    if (!wr(c, a, n, r & m)) return;
    setX(c, rt, old, size == 3);
}

}  // namespace a64
}  // namespace vp
