// a64simd.inc - A64 SIMD and floating-point instructions (C4.1.90, C7):
// the AdvSIMD structure loads and stores, the vector and scalar integer
// operations, permutes, table lookups, and single- and double-precision
// floating point (half precision: conversions only). The arithmetic is
// FpCore's (a64fp.hpp): FPCR's rounding, flush-to-zero and default-NaN
// modes and FPSR's cumulative flags; saturating integer operations set
// FPSR.QC.
// Part of a64machine.hpp.
#pragma once

#include <limits>

#include "a64fp.hpp"

namespace vp {
namespace a64 {

namespace simd {
inline uint64_t el(u128 v, unsigned es, unsigned i) { return (uint64_t)(v >> (i * es)) & (es == 64 ? ~0ull : (1ull << es) - 1); }
inline void setEl(u128& v, unsigned es, unsigned i, uint64_t x) {
    const u128 m = (es == 64 ? (u128)~0ull : (u128)((1ull << es) - 1)) << (i * es);
    v = (v & ~m) | (((u128)x << (i * es)) & m);
}
inline int64_t sx(uint64_t x, unsigned es) { return es == 64 ? (int64_t)x : (int64_t)(x << (64 - es)) >> (64 - es); }
inline uint64_t mask(unsigned es) { return es == 64 ? ~0ull : (1ull << es) - 1; }
inline float f32(uint64_t b) { float f; uint32_t u = (uint32_t)b; memcpy(&f, &u, 4); return f; }
inline double f64(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
inline uint64_t b32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline uint64_t b64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
inline double half2d(uint16_t h) {
    const unsigned s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    double v;
    if (e == 0) v = std::ldexp((double)m, -24);
    else if (e == 31) v = m ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    else v = std::ldexp((double)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
inline uint16_t d2half(double d) {
    // round to nearest even through float's exact range checks
    const uint16_t s = std::signbit(d) ? 0x8000 : 0;
    if (std::isnan(d)) return 0x7e00;
    d = std::fabs(d);
    if (std::isinf(d) || d >= 65520.0) return s | 0x7c00;
    if (d < std::ldexp(1.0, -25)) return s;
    int e;
    std::frexp(d, &e);  // d = f * 2^e, f in [0.5, 1)
    int ex = e - 1;     // d = 1.x * 2^ex
    if (ex < -14) {     // subnormal: units of 2^-24
        const double q = std::nearbyint(std::ldexp(d, 24));
        return s | (uint16_t)q;
    }
    double q = std::nearbyint(std::ldexp(d, 10 - ex));  // 1024..2048
    if (q >= 2048) { q /= 2; ex++; }
    if (ex > 15) return s | 0x7c00;
    return s | (uint16_t)((ex + 15) << 10) | (uint16_t)((uint64_t)q & 1023);
}
}  // namespace simd

// The FP registers as values of type ft (0 single, 1 double, 3 half).
struct Fp {
    static double get(uint64_t bits, unsigned ft) {
        return ft == 0 ? (double)simd::f32(bits) : ft == 1 ? simd::f64(bits) : simd::half2d((uint16_t)bits);
    }
    static uint64_t put(double d, unsigned ft) {
        return ft == 0 ? simd::b32((float)d) : ft == 1 ? simd::b64(d) : simd::d2half(d);
    }
};

}  // namespace a64
}  // namespace vp

namespace vp {
namespace a64 {

// AdvSIMD load/store multiple structures / single structure (C4.1.88.x)
inline void Machine::simdLdSt(Cpu& c, uint32_t i, uint64_t pc) {
    using namespace simd;
    const bool q = i >> 30 & 1, single = i >> 24 & 1, post = i >> 23 & 1, l = i >> 22 & 1;
    const unsigned rm = (i >> 16) & 31, opcode = (i >> 12) & 15, size = (i >> 10) & 3, rn = (i >> 5) & 31, rt = i & 31;
    if (!post && rm) { undefined(c, i, pc); return; }
    uint64_t a = XS(c, rn);
    const uint64_t base = a;
    unsigned total = 0;
    if (!single) {
        unsigned regs, sel;
        switch (opcode) {
            case 0: regs = 4; sel = 4; break;   // LD4/ST4
            case 2: regs = 4; sel = 1; break;   // LD1/ST1 (4 registers)
            case 4: regs = 3; sel = 3; break;   // LD3/ST3
            case 6: regs = 3; sel = 1; break;
            case 7: regs = 1; sel = 1; break;
            case 8: regs = 2; sel = 2; break;   // LD2/ST2
            case 10: regs = 2; sel = 1; break;
            default: undefined(c, i, pc); return;
        }
        if (size == 3 && !q && sel > 1) { undefined(c, i, pc); return; }
        const unsigned es = 8u << size, ebytes = es / 8, elems = (q ? 128 : 64) / es;
        // sel 1: `regs` registers loaded one after the other; else `sel`
        // registers interleaved element by element
        const unsigned nstruct = sel == 1 ? regs : 1;
        const unsigned ilv = sel == 1 ? 1 : sel;
        u128 tmp[4];
        for (unsigned r = 0; r < 4; r++) tmp[r] = c.v[(rt + r) & 31];
        for (unsigned s = 0; s < nstruct; s++)
            for (unsigned e = 0; e < elems; e++)
                for (unsigned k = 0; k < ilv; k++) {
                    const unsigned reg = s + k;
                    if (l) {
                        uint64_t v;
                        if (!rd(c, a, ebytes, v)) return;
                        setEl(tmp[reg], es, e, v);
                    } else if (!wr(c, a, ebytes, el(c.v[(rt + reg) & 31], es, e))) return;
                    a += ebytes;
                }
        if (l)
            for (unsigned r = 0; r < regs; r++) {
                u128 v = tmp[r];
                if (!q) v &= (u128)~0ull;
                c.v[(rt + r) & 31] = v;
            }
        total = (unsigned)(a - base);
    } else {
        const unsigned r = i >> 21 & 1;
        const unsigned sel = ((opcode >> 1 & 1) << 1 | r) + 1;  // structures of 1 to 4 registers
        unsigned es, idx;
        const unsigned scale = opcode >> 2;
        const bool replicate = scale == 3;
        if (replicate) {
            if (!l || (i >> 12 & 1)) { undefined(c, i, pc); return; }
            es = 8u << size;
            idx = 0;
        } else if (scale == 0) { es = 8; idx = (q << 3) | ((i >> 12 & 1) << 2) | size; }
        else if (scale == 1) { if (size & 1) { undefined(c, i, pc); return; } es = 16; idx = (q << 2) | ((i >> 12 & 1) << 1) | (size >> 1); }
        else {
            if (size & 2) { undefined(c, i, pc); return; }
            if (size & 1) { if (i >> 12 & 1) { undefined(c, i, pc); return; } es = 64; idx = q; }
            else { es = 32; idx = (q << 1) | (i >> 12 & 1); }
        }
        const unsigned eb = es / 8;
        for (unsigned k = 0; k < sel; k++) {
            const unsigned reg = (rt + k) & 31;
            if (replicate) {
                uint64_t v;
                if (!rd(c, a, eb, v)) return;
                u128 out = 0;
                for (unsigned e = 0; e < (q ? 128 : 64) / es; e++) setEl(out, es, e, v);
                c.v[reg] = out;
            } else if (l) {
                uint64_t v;
                if (!rd(c, a, eb, v)) return;
                setEl(c.v[reg], es, idx, v);
            } else if (!wr(c, a, eb, el(c.v[reg], es, idx))) return;
            a += eb;
        }
        total = (unsigned)(a - base);
    }
    if (post) setXS(c, rn, base + (rm == 31 ? total : X(c, rm)), true);
}

inline void Machine::simdFp(Cpu& c, uint32_t i, uint64_t pc) {
    using namespace simd;
    if (!fpAllowed(c, pc)) return;
    const unsigned rd = i & 31, rn = (i >> 5) & 31, rm = (i >> 16) & 31;
    auto vdone = [&](u128 v, bool q) { c.v[rd] = q ? v : (v & (u128)~0ull); };
    auto sdone = [&](uint64_t v) { c.v[rd] = v; };  // a scalar result clears the rest
    FpCore fp(c.fpcr, c.fpsr);
    // saturate to es bits, setting FPSR.QC when the value does not fit
    auto sat = [&](__int128 v, unsigned es, bool sgn) -> uint64_t {
        const __int128 hi = sgn ? ((__int128)1 << (es - 1)) - 1 : ((__int128)1 << es) - 1;
        const __int128 lo = sgn ? -((__int128)1 << (es - 1)) : 0;
        if (v > hi) { c.fpsr |= 1u << 27; v = hi; }
        else if (v < lo) { c.fpsr |= 1u << 27; v = lo; }
        return (uint64_t)v & mask(es);
    };

    // ---------------- scalar floating point and conversions (x0x11110 / x0x11111)
    auto fsz = [](unsigned ft) { return ft == 1 ? ~0ull : ft == 0 ? 0xffffffffull : 0xffffull; };
    if ((i & 0x5f000000) == 0x1e000000 && (i >> 21 & 1)) {
        const unsigned ft = (i >> 22) & 3;
        const unsigned m = i >> 31, s = i >> 29 & 1;
        const unsigned b1510 = (i >> 10) & 63;
        if (b1510 == 0) {  // conversion between floating point and integer
            const bool sf = m;
            const unsigned rmode = (i >> 19) & 3, op = (i >> 16) & 7;
            if (s) { undefined(c, i, pc); return; }
            if (ft == 2) {  // FMOV Xd, Vn.D[1] (opcode 110) and Vd.D[1], Xn (111): sf 1, rmode 01
                if (sf && rmode == 1 && (op == 6 || op == 7)) {
                    if (op == 6) setX(c, rd, (uint64_t)(c.v[rn] >> 64), true);
                    else c.v[rd] = (c.v[rd] & (u128)~0ull) | (u128)X(c, rn) << 64;
                    return;
                }
                undefined(c, i, pc);
                return;
            }
            if (ft == 3) { undefined(c, i, pc); return; }  // half precision arithmetic: not there
            if (op == 6 || op == 7) {  // FMOV general <-> FP
                const unsigned bits = ft == 1 ? 64 : 32;
                if (rmode || (bits == 64) != sf) { undefined(c, i, pc); return; }
                if (op == 6) setX(c, rd, (uint64_t)c.v[rn] & mask(bits), true);
                else sdone(X(c, rn) & mask(bits));
                return;
            }
            const unsigned bits = sf ? 64 : 32;
            if (op == 2 || op == 3) {  // SCVTF, UCVTF
                if (rmode) { undefined(c, i, pc); return; }
                sdone(fp.fromInt(X(c, rn), bits, op == 2, ft));
                return;
            }
            unsigned r;  // FCVT{N,P,M,Z}{S,U}, FCVTA{S,U}
            if (op == 0 || op == 1) r = rmode;
            else if (op == 4 || op == 5) { if (rmode) { undefined(c, i, pc); return; } r = 4; }
            else { undefined(c, i, pc); return; }
            setX(c, rd, fp.toInt((uint64_t)c.v[rn] & fsz(ft), ft, bits, !(op & 1), r), sf);
            return;
        }
        if (m || s || ft == 2) { undefined(c, i, pc); return; }
        const uint64_t a = (uint64_t)c.v[rn] & fsz(ft), b = (uint64_t)c.v[rm] & fsz(ft);
        if ((b1510 & 0x1f) == 0x10) {  // data processing, 1 source
            const unsigned op = (i >> 15) & 63;
            if (ft == 3 && op != 4 && op != 5) { undefined(c, i, pc); return; }
            switch (op) {
                case 0: sdone(a); return;                     // FMOV
                case 1: sdone(fp.abs(a, ft)); return;         // FABS
                case 2: sdone(fp.neg(a, ft)); return;         // FNEG
                case 3: sdone(fp.sqrt(a, ft)); return;        // FSQRT
                case 4: case 5: case 7: {                     // FCVT to single (4), double (5), half (7)
                    const unsigned to = op == 4 ? 0 : op == 5 ? 1 : 3;
                    if (to == ft) { undefined(c, i, pc); return; }
                    sdone(fp.convert(a, ft, to));
                    return;
                }
                case 8: case 9: case 10: case 11: case 12: case 14: case 15: {  // FRINTN, P, M, Z, A, X, I
                    const unsigned r = op == 8 ? 0 : op == 9 ? 1 : op == 10 ? 2 : op == 11 ? 3 : op == 12 ? 4 : fp.rmode();
                    sdone(fp.roundInt(a, ft, r, op == 14));
                    return;
                }
                default: undefined(c, i, pc); return;
            }
        }
        if (ft == 3) { undefined(c, i, pc); return; }
        if ((b1510 & 0xf) == 0x8) {  // FCMP, FCMPE
            const unsigned op2 = i & 31;
            if ((op2 & 7) || ((i >> 14) & 3)) { undefined(c, i, pc); return; }
            c.nzcv = fp.compare(a, (op2 & 8) ? 0 : b, ft, op2 & 16);
            return;
        }
        if ((b1510 & 7) == 4) {  // FMOV (scalar, immediate)
            if ((i >> 5) & 31) { undefined(c, i, pc); return; }
            const unsigned imm8 = (i >> 13) & 0xff;
            // VFPExpandImm: sign, exponent 2^-3 .. 2^4, 4 fraction bits
            const unsigned sgn = imm8 >> 7, b6 = imm8 >> 6 & 1, rest = imm8 & 0x3f;
            double d = (1.0 + (rest & 15) / 16.0) * std::ldexp(1.0, (b6 ? -3 : 1) + (int)((rest >> 4) & 3));
            if (sgn) d = -d;
            sdone(Fp::put(d, ft));
            return;
        }
        if ((b1510 & 3) == 1) {  // FCCMP, FCCMPE
            if (cond(c, (i >> 12) & 15)) c.nzcv = fp.compare(a, b, ft, i >> 4 & 1);
            else c.nzcv = (i & 15) << 28;
            return;
        }
        if ((b1510 & 3) == 2) {  // data processing, 2 sources
            uint64_t r;
            switch ((i >> 12) & 15) {
                case 0: r = fp.mul(a, b, ft); break;
                case 1: r = fp.div(a, b, ft); break;
                case 2: r = fp.add(a, b, ft); break;
                case 3: r = fp.sub(a, b, ft); break;
                case 4: r = fp.max(a, b, ft, false); break;
                case 5: r = fp.min(a, b, ft, false); break;
                case 6: r = fp.max(a, b, ft, true); break;
                case 7: r = fp.min(a, b, ft, true); break;
                case 8: r = fp.neg(fp.mul(a, b, ft), ft); break;  // FNMUL
                default: undefined(c, i, pc); return;
            }
            sdone(r);
            return;
        }
        // FCSEL
        sdone(cond(c, (i >> 12) & 15) ? a : b);
        return;
    }
    if ((i & 0x5f000000) == 0x1f000000) {  // FMADD, FMSUB, FNMADD, FNMSUB
        const unsigned ft = (i >> 22) & 3, o1 = i >> 21 & 1, o0 = i >> 15 & 1, ra = (i >> 10) & 31;
        if (ft >= 2 || (i >> 29 & 7) != 0) { undefined(c, i, pc); return; }
        uint64_t n = (uint64_t)c.v[rn] & fsz(ft), mm = (uint64_t)c.v[rm] & fsz(ft), acc = (uint64_t)c.v[ra] & fsz(ft);
        if (o0 != o1) n = fp.neg(n, ft);  // FMSUB, FNMADD
        if (o1) acc = fp.neg(acc, ft);    // FNMADD, FNMSUB
        sdone(fp.fma(acc, n, mm, ft));
        return;
    }
    if ((i & 0x5f200000) == 0x1e000000) {  // conversion between floating point and fixed point
        const bool sf = i >> 31;
        const unsigned ft = (i >> 22) & 3, rmode = (i >> 19) & 3, op = (i >> 16) & 7, scale = (i >> 10) & 63;
        const unsigned fb = 64 - scale, bits = sf ? 64 : 32;
        if (ft >= 2 || (!sf && scale < 32) || (i >> 29 & 1)) { undefined(c, i, pc); return; }
        if (rmode == 0 && (op == 2 || op == 3)) { sdone(fp.fromInt(X(c, rn), bits, op == 2, ft, fb)); return; }
        if (rmode == 3 && (op == 0 || op == 1)) {
            setX(c, rd, fp.toInt((uint64_t)c.v[rn] & fsz(ft), ft, bits, op == 0, 3, fb), sf);
            return;
        }
        undefined(c, i, pc);
        return;
    }

    // ---------------- AdvSIMD
    const bool q = i >> 30 & 1, u = i >> 29 & 1;
    const unsigned size = (i >> 22) & 3;
    const unsigned dsize = q ? 128 : 64;
    const u128 vn = c.v[rn], vm = c.v[rm];
    const bool scalar = (i >> 28 & 1);  // 01x11110 forms

    // copy: DUP, INS, UMOV, SMOV
    if ((i & 0x9fe08400) == 0x0e000400) {
        const bool op = i >> 29 & 1;
        const unsigned imm5 = (i >> 16) & 31, imm4 = (i >> 11) & 15;
        if (!(imm5 & 15)) { undefined(c, i, pc); return; }
        const unsigned sz = __builtin_ctz(imm5), es = 8u << sz, idx = imm5 >> (sz + 1);
        if (op) {  // INS (element)
            if (!q) { undefined(c, i, pc); return; }
            u128 r = c.v[rd];
            setEl(r, es, idx, el(vn, es, imm4 >> sz));
            c.v[rd] = r;
            return;
        }
        switch (imm4) {
            case 0: {  // DUP (element)
                const uint64_t x = el(vn, es, idx);
                u128 r = 0;
                for (unsigned e = 0; e < dsize / es; e++) setEl(r, es, e, x);
                vdone(r, q);
                return;
            }
            case 1: {  // DUP (general)
                if (sz == 3 && !q) { undefined(c, i, pc); return; }
                const uint64_t x = X(c, rn) & mask(es);
                u128 r = 0;
                for (unsigned e = 0; e < dsize / es; e++) setEl(r, es, e, x);
                vdone(r, q);
                return;
            }
            case 3: {  // INS (general)
                u128 r = c.v[rd];
                setEl(r, es, idx, X(c, rn) & mask(es));
                c.v[rd] = r;
                return;
            }
            case 5: {  // SMOV
                const uint64_t x = (uint64_t)sx(el(vn, es, idx), es);
                setX(c, rd, x, q);
                return;
            }
            case 7: {  // UMOV
                setX(c, rd, el(vn, es, idx), q);
                return;
            }
            default: undefined(c, i, pc); return;
        }
    }
    // scalar DUP (element): MOV Bd/Hd/Sd/Dd, Vn.T[i]
    if ((i & 0xffe0fc00) == 0x5e000400) {
        const unsigned imm5 = (i >> 16) & 31;
        if (!(imm5 & 15)) { undefined(c, i, pc); return; }
        const unsigned sz = __builtin_ctz(imm5), es = 8u << sz, idx = imm5 >> (sz + 1);
        sdone(el(vn, es, idx));
        return;
    }
    // modified immediate: MOVI, MVNI, ORR, BIC, FMOV (vector)
    if ((i & 0x9ff80400) == 0x0f000400) {
        const unsigned op = i >> 29 & 1, cmode = (i >> 12) & 15, o2 = i >> 11 & 1;
        const uint64_t imm8 = ((i >> 16) & 7) << 5 | ((i >> 5) & 31);
        if (o2) { undefined(c, i, pc); return; }
        uint64_t imm;
        bool orr = false, bic = false;
        switch (cmode >> 1) {
            case 0: case 1: case 2: case 3: {
                imm = imm8 << (8 * (cmode >> 1));
                imm = imm | imm << 32;
                if (cmode & 1) { if (op) bic = true; else orr = true; }
                else if (op) imm = ~imm;
                break;
            }
            case 4: case 5: {
                imm = imm8 << (8 * ((cmode >> 1) & 1));
                imm = (imm & 0xffff) * 0x0001000100010001ull;
                if (cmode & 1) { if (op) bic = true; else orr = true; }
                else if (op) imm = ~imm;
                break;
            }
            case 6: {
                imm = (cmode & 1) ? (imm8 << 16 | 0xffff) : (imm8 << 8 | 0xff);
                imm = imm | imm << 32;
                if (op) imm = ~imm;
                break;
            }
            default:
                if (cmode == 14) {
                    if (!op) imm = imm8 * 0x0101010101010101ull;
                    else {
                        imm = 0;
                        for (unsigned b = 0; b < 8; b++) if (imm8 >> b & 1) imm |= 0xffull << (8 * b);
                    }
                } else {  // cmode 15: FMOV (vector, immediate)
                    const unsigned sgn = (unsigned)(imm8 >> 7), b6 = (unsigned)(imm8 >> 6 & 1), rest = (unsigned)(imm8 & 0x3f);
                    double d = (1.0 + (rest & 15) / 16.0) * std::ldexp(1.0, b6 ? -3 + (int)((rest >> 4) & 3) : 1 + (int)((rest >> 4) & 3));
                    if (sgn) d = -d;
                    if (!op) { imm = b32((float)d); imm |= imm << 32; }
                    else { if (!q) { undefined(c, i, pc); return; } imm = b64(d); }
                }
                break;
        }
        const u128 v = (u128)imm << 64 | imm;
        if (orr) vdone(c.v[rd] | v, q);
        else if (bic) vdone(c.v[rd] & ~v, q);
        else vdone(v, q);
        return;
    }
    // EXT
    if ((i & 0xbfe08400) == 0x2e000000) {
        const unsigned pos = (i >> 11) & 15;
        if (!q && pos > 7) { undefined(c, i, pc); return; }
        const unsigned bytes = q ? 16 : 8;
        uint8_t b[32];
        for (unsigned k = 0; k < bytes; k++) { b[k] = (uint8_t)el(vn, 8, k); b[k + bytes] = (uint8_t)el(vm, 8, k); }
        u128 r = 0;
        for (unsigned k = 0; k < bytes; k++) setEl(r, 8, k, b[k + pos]);
        vdone(r, q);
        return;
    }
    // TBL, TBX
    if ((i & 0xbfe08c00) == 0x0e000000) {
        const unsigned len = ((i >> 13) & 3) + 1;
        const bool tbx = i >> 12 & 1;
        u128 r = tbx ? c.v[rd] : 0;
        for (unsigned k = 0; k < (q ? 16u : 8u); k++) {
            const unsigned idx = (unsigned)el(vm, 8, k);
            if (idx < 16 * len) setEl(r, 8, k, el(c.v[(rn + idx / 16) & 31], 8, idx % 16));
            else if (!tbx) setEl(r, 8, k, 0);
        }
        vdone(r, q);
        return;
    }
    // permute: UZP, TRN, ZIP
    if ((i & 0xbf208c00) == 0x0e000800) {
        const unsigned op = (i >> 12) & 7, es = 8u << size, n = dsize / es;
        if (size == 3 && !q) { undefined(c, i, pc); return; }
        u128 r = 0;
        const unsigned part = op >> 2;
        for (unsigned e = 0; e < n; e++) {
            uint64_t x;
            switch (op & 3) {
                case 1: {  // UZP1/2
                    const unsigned k = 2 * e + part;
                    x = k < n ? el(vn, es, k) : el(vm, es, k - n);
                    break;
                }
                case 2: {  // TRN1/2
                    x = (e & 1) ? el(vm, es, (e & ~1u) + part) : el(vn, es, (e & ~1u) + part);
                    break;
                }
                case 3: {  // ZIP1/2
                    const unsigned base = part * n / 2;
                    x = (e & 1) ? el(vm, es, base + e / 2) : el(vn, es, base + e / 2);
                    break;
                }
                default: undefined(c, i, pc); return;
            }
            setEl(r, es, e, x);
        }
        vdone(r, q);
        return;
    }
    // shift by immediate (vector and scalar)
    if ((i & 0x9f800400) == 0x0f000400 || (i & 0xdf800400) == 0x5f000400) {
        const unsigned immh = (i >> 19) & 15, immb = (i >> 16) & 7, op = (i >> 11) & 31;
        if (!immh) { undefined(c, i, pc); return; }
        const unsigned hb = 31 - __builtin_clz(immh), es = 8u << hb;
        const unsigned sh = (immh << 3 | immb);
        const unsigned right = 2 * es - sh, left = sh - es;
        const bool sc = scalar;
        const bool narrow = op >= 0x10 && op <= 0x13;
        const bool satLeft = op == 0x0c || op == 0x0e;
        if (narrow) {
            // SHRN, RSHRN (U 0, op 10/11), SQSHRUN, SQRSHRUN (U 1, 10/11),
            // SQSHRN, SQRSHRN, UQSHRN, UQRSHRN (12/13); es is the narrow size
            if (es == 64 || (sc && op <= 0x11 && !u)) { undefined(c, i, pc); return; }
            const unsigned ws = 2 * es, nn = sc ? 1 : 64 / es;
            const bool rnd = op & 1;
            u128 out = (q && !sc) ? c.v[rd] & (u128)~0ull : 0;
            for (unsigned e = 0; e < nn; e++) {
                const uint64_t x = el(vn, ws, e);
                __int128 t = (op >= 0x12 && u) || (op <= 0x11 && !u) ? (__int128)x : (__int128)sx(x, ws);
                if (rnd) t += (__int128)1 << (right - 1);
                t >>= right;
                uint64_t z;
                if (op <= 0x11 && !u) z = (uint64_t)t & mask(es);   // SHRN, RSHRN
                else z = sat(t, es, op >= 0x12 ? !u : false);
                setEl(out, es, (q && !sc ? nn : 0) + e, z);
            }
            if (sc) sdone((uint64_t)out); else c.v[rd] = out;
            return;
        }
        if (sc && es != 64 && op != 0x1c && op != 0x1f && !satLeft) { undefined(c, i, pc); return; }
        const unsigned n = sc ? 1 : dsize / es;
        if (es == 64 && !q && !sc && op != 0x14) { undefined(c, i, pc); return; }
        u128 r = 0;
        switch (op) {
            case 0x00: case 0x02: case 0x04: case 0x06: {  // SSHR/USHR, SSRA/USRA, SRSHR/URSHR, SRSRA/URSRA
                const bool acc = op & 2, rnd = op & 4;
                for (unsigned e = 0; e < n; e++) {
                    const uint64_t x = el(vn, es, e);
                    uint64_t y;
                    if (u) {
                        unsigned __int128 t = (unsigned __int128)x + (rnd ? ((unsigned __int128)1 << (right - 1)) : 0);
                        y = right >= 64 && !rnd ? 0 : (uint64_t)(t >> right);
                    } else {
                        __int128 t = (__int128)sx(x, es) + (rnd ? ((__int128)1 << (right - 1)) : 0);
                        y = (uint64_t)(t >> std::min(right, 127u));
                    }
                    if (acc) y += el(c.v[rd], es, e);
                    setEl(r, es, e, y & mask(es));
                }
                break;
            }
            case 0x08: {  // SRI (U = 1)
                if (!u) { undefined(c, i, pc); return; }
                const uint64_t keep = right >= 64 ? mask(es) : ~(mask(es) >> right) & mask(es);
                for (unsigned e = 0; e < n; e++) {
                    const uint64_t x = right >= 64 ? 0 : el(vn, es, e) >> right;
                    setEl(r, es, e, (el(c.v[rd], es, e) & keep) | x);
                }
                break;
            }
            case 0x0a: {  // SHL (U = 0), SLI (U = 1)
                for (unsigned e = 0; e < n; e++) {
                    const uint64_t x = (el(vn, es, e) << left) & mask(es);
                    const uint64_t keep = u ? (mask(es) >> (es - left)) & (left ? mask(es) : 0) : 0;
                    setEl(r, es, e, u ? ((el(c.v[rd], es, e) & keep) | x) : x);
                }
                break;
            }
            case 0x0c: case 0x0e: {  // SQSHLU (U 1, op 0c); SQSHL, UQSHL (op 0e)
                if (op == 0x0c && !u) { undefined(c, i, pc); return; }
                for (unsigned e = 0; e < n; e++) {
                    const uint64_t x = el(vn, es, e);
                    const bool srcSigned = op == 0x0c || !u;
                    const __int128 t = (srcSigned ? (__int128)sx(x, es) : (__int128)x) * ((__int128)1 << left);
                    setEl(r, es, e, sat(t, es, op == 0x0e && !u));
                }
                break;
            }
            case 0x14: {  // SSHLL, USHLL (UXTL, SXTL)
                if (es == 64 || sc) { undefined(c, i, pc); return; }
                const unsigned ws = 2 * es, nn = 64 / es;
                for (unsigned e = 0; e < nn; e++) {
                    const uint64_t x = el(vn, es, (q ? nn : 0) + e);
                    const uint64_t y = (u ? x : (uint64_t)sx(x, es)) << left;
                    setEl(r, ws, e, y & mask(ws));
                }
                c.v[rd] = r;
                return;
            }
            case 0x1c: case 0x1f: {  // SCVTF/UCVTF, FCVTZS/FCVTZU (fixed point)
                if (es < 32) { undefined(c, i, pc); return; }
                const unsigned ft = es == 64 ? 1 : 0;
                for (unsigned e = 0; e < n; e++) {
                    const uint64_t x = el(vn, es, e);
                    if (op == 0x1c) setEl(r, es, e, fp.fromInt(x, es, !u, ft, right));
                    else setEl(r, es, e, fp.toInt(x, ft, es, !u, 3, right));
                }
                break;
            }
            default: undefined(c, i, pc); return;
        }
        if (sc) sdone((uint64_t)r); else vdone(r, q);
        return;
    }
    // three registers of the same type (vector and scalar)
    if ((i & 0x9f200400) == 0x0e200400 || (i & 0xdf200400) == 0x5e200400) {
        const unsigned op = (i >> 11) & 31;
        if (op >= 0x18) {  // floating point
            const unsigned a = size >> 1, ft = size & 1, es = ft ? 64 : 32;
            if (ft && !q && !scalar) { undefined(c, i, pc); return; }
            const unsigned n = scalar ? 1 : dsize / es;
            const unsigned key = u << 6 | a << 5 | op;
            const bool pairwise = u && !scalar && (key == 0x5a || key == 0x5e || key == 0x7e || key == 0x58 || key == 0x78);
            if (scalar && (pairwise || key == 0x19 || key == 0x39 || key == 0x5a || key == 0x5b || key == 0x5f || key == 0x18 ||
                           key == 0x38 || key == 0x1a || key == 0x3a || key == 0x1e || key == 0x3e)) { undefined(c, i, pc); return; }
            u128 r = 0;
            for (unsigned e = 0; e < n; e++) {
                uint64_t x, y;
                if (pairwise) {
                    const unsigned k = 2 * e;
                    x = k < n ? el(vn, es, k) : el(vm, es, k - n);
                    y = k + 1 < n ? el(vn, es, k + 1) : el(vm, es, k + 1 - n);
                } else { x = el(vn, es, e); y = el(vm, es, e); }
                uint64_t z;
                switch (key) {
                    case 0x1a: case 0x5a: z = fp.add(x, y, ft); break;              // FADD, FADDP
                    case 0x3a: z = fp.sub(x, y, ft); break;                         // FSUB
                    case 0x7a: z = fp.abd(x, y, ft); break;                         // FABD
                    case 0x5b: z = fp.mul(x, y, ft); break;                         // FMUL
                    case 0x1b: z = fp.mulx(x, y, ft); break;                        // FMULX
                    case 0x5f: z = fp.div(x, y, ft); break;                         // FDIV
                    case 0x19: z = fp.fma(el(c.v[rd], es, e), x, y, ft); break;     // FMLA
                    case 0x39: z = fp.fma(el(c.v[rd], es, e), fp.neg(x, ft), y, ft); break;  // FMLS
                    case 0x1e: case 0x5e: z = fp.max(x, y, ft, false); break;       // FMAX(P)
                    case 0x3e: case 0x7e: z = fp.min(x, y, ft, false); break;       // FMIN(P)
                    case 0x18: case 0x58: z = fp.max(x, y, ft, true); break;        // FMAXNM(P)
                    case 0x38: case 0x78: z = fp.min(x, y, ft, true); break;        // FMINNM(P)
                    case 0x1c: z = fp.cmp(x, y, ft, 0) ? mask(es) : 0; break;       // FCMEQ
                    case 0x5c: z = fp.cmp(x, y, ft, 1) ? mask(es) : 0; break;       // FCMGE
                    case 0x7c: z = fp.cmp(x, y, ft, 2) ? mask(es) : 0; break;       // FCMGT
                    case 0x5d: z = fp.cmp(fp.abs(x, ft), fp.abs(y, ft), ft, 1) ? mask(es) : 0; break;  // FACGE
                    case 0x7d: z = fp.cmp(fp.abs(x, ft), fp.abs(y, ft), ft, 2) ? mask(es) : 0; break;  // FACGT
                    case 0x1f: z = fp.recps(x, y, ft); break;                       // FRECPS
                    case 0x3f: z = fp.rsqrts(x, y, ft); break;                      // FRSQRTS
                    default: undefined(c, i, pc); return;
                }
                setEl(r, es, e, z);
            }
            if (scalar) sdone((uint64_t)r); else vdone(r, q);
            return;
        }
        const unsigned es = 8u << size;
        if (size == 3 && !q && !scalar && op != 3) { undefined(c, i, pc); return; }
        if (op == 3) {  // logical
            u128 r;
            const u128 d = c.v[rd];
            switch (u << 2 | size) {
                case 0: r = vn & vm; break;
                case 1: r = vn & ~vm; break;
                case 2: r = vn | vm; break;
                case 3: r = vn | ~vm; break;
                case 4: r = vn ^ vm; break;
                case 5: r = (d & vn) | (~d & vm); break;   // BSL
                case 6: r = (d & ~vm) | (vn & vm); break;  // BIT
                default: r = (d & vm) | (vn & ~vm); break;  // BIF
            }
            vdone(r, q);
            return;
        }
        if (scalar && !(op == 0x01 || op == 0x05 || op == 0x09 || op == 0x0b || op == 0x16) &&
            !(size == 3 && (op == 0x06 || op == 0x07 || op == 0x08 || op == 0x0a || op == 0x10 || op == 0x11))) { undefined(c, i, pc); return; }
        if (op == 0x16 && (size == 0 || size == 3)) { undefined(c, i, pc); return; }
        if ((op == 0x13 || op == 0x12 || op == 0x14 || op == 0x15 || op == 0x0c || op == 0x0d || op == 0x0e || op == 0x0f || op <= 0x04) && size == 3 && op != 0x01) { undefined(c, i, pc); return; }
        const unsigned n = scalar ? 1 : dsize / es;
        u128 r = 0;
        for (unsigned e = 0; e < n; e++) {
            uint64_t x, y;
            const bool pairwise = !scalar && (op == 0x14 || op == 0x15 || op == 0x17);
            if (pairwise) {
                const unsigned k = 2 * e;
                x = k < n ? el(vn, es, k) : el(vm, es, k - n);
                y = k + 1 < n ? el(vn, es, k + 1) : el(vm, es, k + 1 - n);
            } else { x = el(vn, es, e); y = el(vm, es, e); }
            const int64_t sx_ = sx(x, es), sy = sx(y, es);
            uint64_t z;
            switch (op) {
                case 0x00: z = u ? (uint64_t)(((unsigned __int128)x + y) >> 1) : (uint64_t)(((__int128)sx_ + sy) >> 1); break;  // HADD
                case 0x02: z = u ? (uint64_t)(((unsigned __int128)x + y + 1) >> 1) : (uint64_t)(((__int128)sx_ + sy + 1) >> 1); break;  // RHADD
                case 0x04: z = u ? (uint64_t)(((__int128)x - (__int128)y) >> 1) : (uint64_t)(((__int128)sx_ - sy) >> 1); break;  // HSUB
                case 0x01:  // SQADD, UQADD
                    z = u ? sat((__int128)x + y, es, false) : sat((__int128)sx_ + sy, es, true);
                    break;
                case 0x05:  // SQSUB, UQSUB
                    z = u ? sat((__int128)x - (__int128)y, es, false) : sat((__int128)sx_ - sy, es, true);
                    break;
                case 0x09: case 0x0b: {  // SQSHL, UQSHL, SQRSHL, UQRSHL (register)
                    const int sh = (int8_t)y;
                    const __int128 v = u ? (__int128)x : (__int128)sx_;
                    if (sh >= 0) {
                        if (v == 0) z = 0;
                        else if (sh >= 64) z = sat(v < 0 ? -((__int128)1 << 100) : ((__int128)1 << 100), es, !u);
                        else z = sat(v * ((__int128)1 << sh), es, !u);
                    } else {
                        const int k = -sh;
                        __int128 t;
                        if (op == 0x0b) t = k >= 127 ? 0 : (v + ((__int128)1 << (k - 1))) >> k;
                        else t = k >= 127 ? (v < 0 ? -1 : 0) : v >> k;
                        z = (uint64_t)t & mask(es);
                    }
                    break;
                }
                case 0x16: {  // SQDMULH, SQRDMULH
                    const __int128 t = 2 * (__int128)sx_ * sy + (u ? (__int128)1 << (es - 1) : 0);
                    z = sat(t >> es, es, true);
                    break;
                }
                case 0x06: z = (u ? x > y : sx_ > sy) ? ~0ull : 0; break;    // CMGT, CMHI
                case 0x07: z = (u ? x >= y : sx_ >= sy) ? ~0ull : 0; break;  // CMGE, CMHS
                case 0x08: {  // SSHL, USHL
                    const int8_t s = (int8_t)y;
                    if (s >= 0) z = s >= (int)es ? 0 : x << s;
                    else z = u ? (-s >= (int)es ? 0 : x >> -s) : (uint64_t)(sx_ >> std::min(-s, (int)es - 1));
                    break;
                }
                case 0x0a: {  // SRSHL, URSHL
                    const int8_t s = (int8_t)y;
                    if (s >= 0) z = s >= (int)es ? 0 : x << s;
                    else {
                        const int k = -s;
                        if (u) z = k > (int)es ? 0 : (uint64_t)(((unsigned __int128)x + ((unsigned __int128)1 << (k - 1))) >> k);
                        else z = k > (int)es ? 0 : (uint64_t)(((__int128)sx_ + ((__int128)1 << (k - 1))) >> k);
                    }
                    break;
                }
                case 0x0c: case 0x14: z = u ? std::max(x, y) : (uint64_t)std::max(sx_, sy); break;  // MAX, MAXP
                case 0x0d: case 0x15: z = u ? std::min(x, y) : (uint64_t)std::min(sx_, sy); break;  // MIN, MINP
                case 0x0e: z = u ? (x > y ? x - y : y - x) : (uint64_t)(sx_ > sy ? sx_ - sy : sy - sx_); break;  // ABD
                case 0x0f: z = el(c.v[rd], es, e) + (u ? (x > y ? x - y : y - x) : (uint64_t)(sx_ > sy ? sx_ - sy : sy - sx_)); break;  // ABA
                case 0x10: z = u ? x - y : x + y; break;                     // ADD, SUB
                case 0x11: z = (u ? x == y : (x & y) != 0) ? ~0ull : 0; break;  // CMEQ, CMTST
                case 0x12: z = u ? el(c.v[rd], es, e) - x * y : el(c.v[rd], es, e) + x * y; break;  // MLA, MLS
                case 0x13: {  // MUL, PMUL
                    if (!u) z = x * y;
                    else { z = 0; for (unsigned b = 0; b < es; b++) if (y >> b & 1) z ^= x << b; }
                    break;
                }
                case 0x17: if (u) { undefined(c, i, pc); return; } z = x + y; break;  // ADDP
                default: undefined(c, i, pc); return;
            }
            setEl(r, es, e, z & mask(es));
        }
        if (scalar) sdone((uint64_t)r); else vdone(r, q);
        return;
    }
    // three registers of different types
    if ((i & 0x9f200c00) == 0x0e200000 || (i & 0xdf200c00) == 0x5e200000) {
        const unsigned op = (i >> 12) & 15, es = 8u << size, ws = 2 * es;
        if (size == 3 && op != 14) { undefined(c, i, pc); return; }
        if (scalar && op != 9 && op != 11 && op != 13) { undefined(c, i, pc); return; }
        const unsigned nn = scalar ? 1 : 64 / es, half = q && !scalar ? nn : 0;
        u128 r = 0;
        if (op == 9 || op == 11 || op == 13) {  // SQDMLAL, SQDMLSL, SQDMULL
            if (u || size == 0) { undefined(c, i, pc); return; }
            for (unsigned e = 0; e < nn; e++) {
                const __int128 p = sat(2 * (__int128)sx(el(vn, es, half + e), es) * sx(el(vm, es, half + e), es), ws, true);
                uint64_t z = (uint64_t)p & mask(ws);
                if (op == 9) z = sat((__int128)sx(el(c.v[rd], ws, e), ws) + sx(z, ws), ws, true);
                else if (op == 11) z = sat((__int128)sx(el(c.v[rd], ws, e), ws) - sx(z, ws), ws, true);
                setEl(r, ws, e, z);
            }
            if (scalar) sdone((uint64_t)r); else c.v[rd] = r;
            return;
        }
        if (op == 4 || op == 6) {  // ADDHN, SUBHN (RADDHN, RSUBHN with U)
            u128 out = q ? c.v[rd] & (u128)~0ull : 0;
            for (unsigned e = 0; e < nn; e++) {
                unsigned __int128 t = op == 4 ? (unsigned __int128)el(vn, ws, e) + el(vm, ws, e) : (unsigned __int128)el(vn, ws, e) - el(vm, ws, e);
                if (u) t += (unsigned __int128)1 << (es - 1);
                setEl(out, es, half + e, (uint64_t)(t >> es) & mask(es));
            }
            c.v[rd] = out;
            return;
        }
        if (op == 14 && !u) {  // PMULL (8-bit and 64-bit)
            if (size == 0) {
                for (unsigned e = 0; e < 8; e++) {
                    const uint64_t x = el(vn, 8, half + e), y = el(vm, 8, half + e);
                    uint64_t z = 0;
                    for (unsigned b = 0; b < 8; b++) if (y >> b & 1) z ^= x << b;
                    setEl(r, 16, e, z);
                }
                c.v[rd] = r;
                return;
            }
            undefined(c, i, pc);
            return;
        }
        for (unsigned e = 0; e < nn; e++) {
            const uint64_t a = op == 1 || op == 3 ? el(vn, ws, e) : el(vn, es, half + e);
            const uint64_t b = el(vm, es, half + e);
            const int64_t sa = op == 1 || op == 3 ? sx(a, ws) : sx(a, es), sb = sx(b, es);
            const uint64_t ea = u ? a : (uint64_t)sa, eb = u ? b : (uint64_t)sb;
            uint64_t z;
            switch (op) {
                case 0: case 1: z = ea + eb; break;                                // ADDL, ADDW
                case 2: case 3: z = ea - eb; break;                                // SUBL, SUBW
                case 5: z = el(c.v[rd], ws, e) + (u ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa)); break;  // ABAL
                case 7: z = u ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa); break;  // ABDL
                case 8: z = el(c.v[rd], ws, e) + ea * eb; break;                   // MLAL
                case 10: z = el(c.v[rd], ws, e) - ea * eb; break;                  // MLSL
                case 12: z = ea * eb; break;                                       // MULL
                default: undefined(c, i, pc); return;
            }
            setEl(r, ws, e, z & mask(ws));
        }
        c.v[rd] = r;
        return;
    }
    // two-register miscellaneous (vector and scalar)
    if ((i & 0x9f3e0c00) == 0x0e200800 || (i & 0xdf3e0c00) == 0x5e200800) {
        const unsigned op = (i >> 12) & 31, es = 8u << size;
        const unsigned n = scalar ? 1 : dsize / es;
        u128 r = 0;
        // floating point forms (size<1> selects some)
        if (op >= 0x0c && op != 0x12 && op != 0x13 && op != 0x14) {
            const unsigned ft = size & 1, fes = ft ? 64 : 32, fn = scalar ? 1 : dsize / fes;
            const unsigned key = u << 6 | (size >> 1) << 5 | op;
            if (key == 0x16 || key == 0x56 || key == 0x17) {  // FCVTN, FCVTXN (narrow), FCVTL (widen)
                const unsigned nes = ft ? 32 : 16, nn = ft ? 2 : 4, nt = ft ? 0 : 3, wt = ft ? 1 : 0;
                if (key == 0x56 && !ft) { undefined(c, i, pc); return; }
                if (scalar && key != 0x56) { undefined(c, i, pc); return; }
                if (op == 0x16) {
                    const unsigned cnt = scalar ? 1 : nn, at = q && !scalar ? nn : 0;
                    u128 out = q && !scalar ? c.v[rd] & (u128)~0ull : 0;
                    for (unsigned e = 0; e < cnt; e++)
                        setEl(out, nes, at + e, fp.convert(el(vn, 2 * nes, e), wt, nt, key == 0x56 ? 5 : -1));
                    if (scalar) sdone((uint64_t)out); else c.v[rd] = out;
                } else {
                    for (unsigned e = 0; e < nn; e++) setEl(r, 2 * nes, e, fp.convert(el(vn, nes, (q ? nn : 0) + e), nt, wt));
                    c.v[rd] = r;
                }
                return;
            }
            if (ft && !q && !scalar) { undefined(c, i, pc); return; }
            if (scalar && (key == 0x2f || key == 0x6f || key == 0x7f || (op >= 0x18 && op <= 0x19) || key == 0x3c || key == 0x7c)) { undefined(c, i, pc); return; }
            if (!scalar && key == 0x3f) { undefined(c, i, pc); return; }
            for (unsigned e = 0; e < fn; e++) {
                const uint64_t x = el(vn, fes, e);
                uint64_t z;
                switch (key) {
                    case 0x2f: z = fp.abs(x, ft); break;    // FABS
                    case 0x6f: z = fp.neg(x, ft); break;    // FNEG
                    case 0x7f: z = fp.sqrt(x, ft); break;   // FSQRT
                    case 0x1d: z = fp.fromInt(x, fes, true, ft); break;   // SCVTF
                    case 0x5d: z = fp.fromInt(x, fes, false, ft); break;  // UCVTF
                    case 0x1a: z = fp.toInt(x, ft, fes, true, 0); break;   // FCVTNS
                    case 0x1b: z = fp.toInt(x, ft, fes, true, 2); break;   // FCVTMS
                    case 0x1c: z = fp.toInt(x, ft, fes, true, 4); break;   // FCVTAS
                    case 0x3a: z = fp.toInt(x, ft, fes, true, 1); break;   // FCVTPS
                    case 0x3b: z = fp.toInt(x, ft, fes, true, 3); break;   // FCVTZS
                    case 0x5a: z = fp.toInt(x, ft, fes, false, 0); break;  // FCVTNU
                    case 0x5b: z = fp.toInt(x, ft, fes, false, 2); break;  // FCVTMU
                    case 0x5c: z = fp.toInt(x, ft, fes, false, 4); break;  // FCVTAU
                    case 0x7a: z = fp.toInt(x, ft, fes, false, 1); break;  // FCVTPU
                    case 0x7b: z = fp.toInt(x, ft, fes, false, 3); break;  // FCVTZU
                    case 0x18: z = fp.roundInt(x, ft, 0, false); break;    // FRINTN
                    case 0x38: z = fp.roundInt(x, ft, 1, false); break;    // FRINTP
                    case 0x19: z = fp.roundInt(x, ft, 2, false); break;    // FRINTM
                    case 0x39: z = fp.roundInt(x, ft, 3, false); break;    // FRINTZ
                    case 0x58: z = fp.roundInt(x, ft, 4, false); break;    // FRINTA
                    case 0x59: z = fp.roundInt(x, ft, fp.rmode(), true); break;   // FRINTX
                    case 0x79: z = fp.roundInt(x, ft, fp.rmode(), false); break;  // FRINTI
                    case 0x2c: z = fp.cmp(x, 0, ft, 2) ? mask(fes) : 0; break;    // FCMGT #0
                    case 0x6c: z = fp.cmp(x, 0, ft, 1) ? mask(fes) : 0; break;    // FCMGE #0
                    case 0x2d: z = fp.cmp(x, 0, ft, 0) ? mask(fes) : 0; break;    // FCMEQ #0
                    case 0x6d: z = fp.cmp(0, x, ft, 1) ? mask(fes) : 0; break;    // FCMLE #0
                    case 0x2e: z = fp.cmp(0, x, ft, 2) ? mask(fes) : 0; break;    // FCMLT #0
                    case 0x3d: z = fp.recipEstimate(x, ft); break;   // FRECPE
                    case 0x7d: z = fp.rsqrtEstimate(x, ft); break;   // FRSQRTE
                    case 0x3f: z = fp.recpX(x, ft); break;           // FRECPX
                    case 0x3c: if (ft) { undefined(c, i, pc); return; } z = FpCore::urecpe((uint32_t)x); break;   // URECPE
                    case 0x7c: if (ft) { undefined(c, i, pc); return; } z = FpCore::ursqrte((uint32_t)x); break;  // URSQRTE
                    default: undefined(c, i, pc); return;
                }
                setEl(r, fes, e, z & mask(fes));
            }
            if (scalar) sdone((uint64_t)r); else vdone(r, q);
            return;
        }
        if (op == 0x12 || op == 0x14) {  // XTN, SQXTUN (op 0x12, U), SQXTN / UQXTN (0x14)
            if (size == 3 || (scalar && op == 0x12 && !u)) { undefined(c, i, pc); return; }
            const unsigned ws = 2 * es, nn = 64 / es;
            u128 out = (q && !scalar) ? c.v[rd] & (u128)~0ull : 0;
            for (unsigned e = 0; e < (scalar ? 1 : nn); e++) {
                const uint64_t x = el(vn, ws, e);
                uint64_t z;
                if (op == 0x12 && !u) z = x & mask(es);
                else if (op == 0x12) z = sat(sx(x, ws), es, false);  // SQXTUN
                else if (u) z = sat(x, es, false);                   // UQXTN
                else z = sat(sx(x, ws), es, true);                   // SQXTN
                setEl(out, es, (q && !scalar ? nn : 0) + e, z);
            }
            if (scalar) sdone((uint64_t)out); else c.v[rd] = out;
            return;
        }
        if (scalar && !(op == 0x03 || op == 0x07 || (size == 3 && (op == 0x08 || op == 0x09 || op == 0x0a || op == 0x0b)))) { undefined(c, i, pc); return; }
        if (op == 0x05 && (u || size == 0)) {  // CNT, NOT (size 0); RBIT (U 1, size 1)
            if (!u && size) { undefined(c, i, pc); return; }
            if (u && size > 1) { undefined(c, i, pc); return; }
            for (unsigned e = 0; e < dsize / 8; e++) {
                const uint64_t x = el(vn, 8, e);
                uint64_t z;
                if (!u) z = (uint64_t)__builtin_popcountll(x);
                else if (size == 0) z = ~x & 0xff;
                else { z = 0; for (unsigned b = 0; b < 8; b++) if (x >> b & 1) z |= 1ull << (7 - b); }
                setEl(r, 8, e, z);
            }
            vdone(r, q);
            return;
        }
        if (op == 0x03 || op == 0x07) {  // SUQADD, USQADD; SQABS, SQNEG
            if (size == 3 && !q && !scalar) { undefined(c, i, pc); return; }
            for (unsigned e = 0; e < n; e++) {
                const uint64_t x = el(vn, es, e);
                uint64_t z;
                if (op == 0x03) z = u ? sat((__int128)el(c.v[rd], es, e) + sx(x, es), es, false)   // USQADD
                                      : sat((__int128)sx(el(c.v[rd], es, e), es) + x, es, true);   // SUQADD
                else z = u ? sat(-(__int128)sx(x, es), es, true) : sat(sx(x, es) < 0 ? -(__int128)sx(x, es) : sx(x, es), es, true);
                setEl(r, es, e, z);
            }
            if (scalar) sdone((uint64_t)r); else vdone(r, q);
            return;
        }
        if (op == 0x13 && u) {  // SHLL
            if (size == 3) { undefined(c, i, pc); return; }
            const unsigned ws = 2 * es, nn = 64 / es;
            for (unsigned e = 0; e < nn; e++) setEl(r, ws, e, el(vn, es, (q ? nn : 0) + e) << es);
            c.v[rd] = r;
            return;
        }
        if (op == 0x02 || op == 0x06) {  // SADDLP, UADDLP, SADALP, UADALP
            if (size == 3) { undefined(c, i, pc); return; }
            const unsigned ws = 2 * es, nn = dsize / ws;
            for (unsigned e = 0; e < nn; e++) {
                const uint64_t a = el(vn, es, 2 * e), b = el(vn, es, 2 * e + 1);
                uint64_t z = u ? a + b : (uint64_t)(sx(a, es) + sx(b, es));
                if (op == 6) z += el(c.v[rd], ws, e);
                setEl(r, ws, e, z & mask(ws));
            }
            vdone(r, q);
            return;
        }
        if (size == 3 && !q && !scalar) { undefined(c, i, pc); return; }
        for (unsigned e = 0; e < n; e++) {
            const uint64_t x = el(vn, es, e);
            const int64_t s = sx(x, es);
            uint64_t z;
            switch (op << 1 | u) {
                case 0x00: case 0x01: {  // REV64, REV32
                    const unsigned grp = (op << 1 | u) == 0 ? 64 : 32;
                    if (es >= grp) { undefined(c, i, pc); return; }
                    const unsigned per = grp / es;
                    z = el(vn, es, (e / per) * per + (per - 1 - e % per));
                    break;
                }
                case 0x02: {  // REV16
                    if (es != 8) { undefined(c, i, pc); return; }
                    z = el(vn, 8, e ^ 1);
                    break;
                }
                case 0x08: {  // CLS
                    const uint64_t t = (x ^ (x << 1)) & mask(es);
                    z = t ? (uint64_t)(__builtin_clzll(t) - (64 - es)) : es - 1;
                    break;
                }
                case 0x09: z = x ? (uint64_t)(__builtin_clzll(x) - (64 - es)) : es; break;  // CLZ
                case 0x10: z = s > 0 ? ~0ull : 0; break;   // CMGT #0
                case 0x11: z = s >= 0 ? ~0ull : 0; break;  // CMGE #0
                case 0x12: z = s == 0 ? ~0ull : 0; break;  // CMEQ #0
                case 0x13: z = s <= 0 ? ~0ull : 0; break;  // CMLE #0
                case 0x14: z = s < 0 ? ~0ull : 0; break;   // CMLT #0
                case 0x16: z = (uint64_t)(s < 0 ? -s : s); break;  // ABS
                case 0x17: z = (uint64_t)(-s); break;              // NEG
                default: undefined(c, i, pc); return;
            }
            setEl(r, es, e, z & mask(es));
        }
        if (scalar) sdone((uint64_t)r); else vdone(r, q);
        return;
    }
    // across lanes
    if ((i & 0x9f3e0c00) == 0x0e300800) {
        const unsigned op = (i >> 12) & 31, es = 8u << size, n = dsize / es;
        if (op == 0x0c || op == 0x0f) {  // FMAXNMV, FMAXV, FMINNMV, FMINV (single); a tree: (0,1), (2,3)
            if (!q || (size & 1) || !u) { undefined(c, i, pc); return; }
            const bool mn = size >> 1, nm = op == 0x0c;
            auto f = [&](uint64_t x, uint64_t y) { return mn ? fp.min(x, y, 0, nm) : fp.max(x, y, 0, nm); };
            const uint64_t lo = f(el(vn, 32, 0), el(vn, 32, 1)), hi = f(el(vn, 32, 2), el(vn, 32, 3));
            sdone(f(lo, hi));
            return;
        }
        if (size == 3 || (size == 2 && !q)) { undefined(c, i, pc); return; }
        uint64_t acc = el(vn, es, 0);
        int64_t sacc = sx(acc, es);
        if (op == 3) {  // SADDLV, UADDLV
            uint64_t t = 0;
            for (unsigned e = 0; e < n; e++) t += u ? el(vn, es, e) : (uint64_t)sx(el(vn, es, e), es);
            sdone(t & mask(2 * es));
            return;
        }
        for (unsigned e = 1; e < n; e++) {
            const uint64_t x = el(vn, es, e);
            const int64_t s = sx(x, es);
            switch (op) {
                case 0x0a: if (u) acc = std::max(acc, x); else sacc = std::max(sacc, s); break;  // MAXV
                case 0x1a: if (u) acc = std::min(acc, x); else sacc = std::min(sacc, s); break;  // MINV
                case 0x1b: if (u) { undefined(c, i, pc); return; } acc += x; break;              // ADDV
                default: undefined(c, i, pc); return;
            }
        }
        if ((op == 0x0a || op == 0x1a) && !u) acc = (uint64_t)sacc;
        sdone(acc & mask(es));
        return;
    }
    // scalar pairwise: ADDP (D), FADDP, FMAXP, FMINP, FMAXNMP, FMINNMP
    if ((i & 0xdf3e0c00) == 0x5e300800) {
        const unsigned op = (i >> 12) & 31;
        if (op == 0x1b && !u && size == 3) { sdone(el(vn, 64, 0) + el(vn, 64, 1)); return; }  // ADDP
        if (u) {
            const unsigned ft = size & 1, es = ft ? 64 : 32;
            const uint64_t a = el(vn, es, 0), b = el(vn, es, 1);
            uint64_t z;
            switch ((size >> 1) << 5 | op) {
                case 0x0d: z = fp.add(a, b, ft); break;        // FADDP
                case 0x0f: z = fp.max(a, b, ft, false); break; // FMAXP
                case 0x2f: z = fp.min(a, b, ft, false); break; // FMINP
                case 0x0c: z = fp.max(a, b, ft, true); break;  // FMAXNMP
                case 0x2c: z = fp.min(a, b, ft, true); break;  // FMINNMP
                default: undefined(c, i, pc); return;
            }
            sdone(z);
            return;
        }
        undefined(c, i, pc);
        return;
    }
    // vector x indexed element
    if ((i & 0x9f000400) == 0x0f000000 || (i & 0xdf000400) == 0x5f000000) {
        const unsigned op = (i >> 12) & 15, l = i >> 21 & 1, mbit = i >> 20 & 1, h = i >> 11 & 1;
        unsigned es, idx, mreg = (i >> 16) & 15;
        if (size == 1) { es = 16; idx = h << 2 | l << 1 | mbit; }
        else if (size == 2) { es = 32; idx = h << 1 | l; mreg |= mbit << 4; }
        else if (size == 3) { es = 64; idx = h; mreg |= mbit << 4; if (l) { undefined(c, i, pc); return; } }
        else { undefined(c, i, pc); return; }
        const bool isFp = (op == 1 || op == 5 || op == 9) && !(u && op != 9);
        if (isFp) {  // FMLA, FMLS, FMUL, FMULX (U 1)
            if (es == 16 || (es == 64 && !q && !scalar)) { undefined(c, i, pc); return; }
            const unsigned ft = es == 64 ? 1 : 0, n = scalar ? 1 : dsize / es;
            const uint64_t y = el(c.v[mreg], es, idx);
            u128 r = 0;
            for (unsigned e = 0; e < n; e++) {
                const uint64_t x = el(vn, es, e);
                uint64_t z;
                if (op == 9) z = u ? fp.mulx(x, y, ft) : fp.mul(x, y, ft);
                else z = fp.fma(el(c.v[rd], es, e), op == 5 ? fp.neg(x, ft) : x, y, ft);
                setEl(r, es, e, z);
            }
            if (scalar) sdone((uint64_t)r); else vdone(r, q);
            return;
        }
        if (es == 64) { undefined(c, i, pc); return; }
        const uint64_t y = el(c.v[mreg], es, idx);
        const unsigned n = scalar ? 1 : dsize / es;
        u128 r = 0;
        if (scalar && !(op == 3 || op == 7 || op == 11 || op == 12 || op == 13) ) { undefined(c, i, pc); return; }
        if ((!u && op == 8) || (u && (op == 0 || op == 4))) {  // MUL, MLA, MLS (by element)
            for (unsigned e = 0; e < n; e++) {
                const uint64_t x = el(vn, es, e);
                uint64_t z = x * y;
                if (op == 0) z = el(c.v[rd], es, e) + z;
                else if (op == 4) z = el(c.v[rd], es, e) - z;
                setEl(r, es, e, z & mask(es));
            }
            vdone(r, q);
            return;
        }
        if (!u && (op == 12 || op == 13)) {  // SQDMULH, SQRDMULH (by element)
            for (unsigned e = 0; e < n; e++) {
                const __int128 t = 2 * (__int128)sx(el(vn, es, e), es) * sx(y, es) + (op == 13 ? (__int128)1 << (es - 1) : 0);
                setEl(r, es, e, sat(t >> es, es, true));
            }
            if (scalar) sdone((uint64_t)r); else vdone(r, q);
            return;
        }
        const unsigned ws = 2 * es, nn = scalar ? 1 : 64 / es, half = q && !scalar ? nn : 0;
        if (!u && (op == 3 || op == 7 || op == 11)) {  // SQDMLAL, SQDMLSL, SQDMULL (by element)
            for (unsigned e = 0; e < nn; e++) {
                uint64_t z = sat(2 * (__int128)sx(el(vn, es, half + e), es) * sx(y, es), ws, true);
                if (op == 3) z = sat((__int128)sx(el(c.v[rd], ws, e), ws) + sx(z, ws), ws, true);
                else if (op == 7) z = sat((__int128)sx(el(c.v[rd], ws, e), ws) - sx(z, ws), ws, true);
                setEl(r, ws, e, z);
            }
            if (scalar) sdone((uint64_t)r); else c.v[rd] = r;
            return;
        }
        if (op == 10 || op == 2 || op == 6) {  // SMULL/UMULL, SMLAL/UMLAL, SMLSL/UMLSL (by element)
            const uint64_t ey = u ? y : (uint64_t)sx(y, es);
            for (unsigned e = 0; e < nn; e++) {
                const uint64_t x = el(vn, es, half + e);
                const uint64_t ex = u ? x : (uint64_t)sx(x, es);
                uint64_t z = ex * ey;
                if (op == 2) z = el(c.v[rd], ws, e) + z;
                else if (op == 6) z = el(c.v[rd], ws, e) - z;
                setEl(r, ws, e, z & mask(ws));
            }
            c.v[rd] = r;
            return;
        }
        undefined(c, i, pc);
        return;
    }
    // AES, SHA: not there (ID_AA64ISAR0_EL1 says so)
    undefined(c, i, pc);
}

}  // namespace a64
}  // namespace vp
