// a64fp.hpp - floating-point arithmetic as the Arm architecture defines it
// (Arm ARM DDI 0487, the FP* pseudocode functions of chapter J1): NaN
// propagation and the default NaN (FPCR.DN), flush to zero (FPCR.FZ) with
// tininess detected before rounding, the four rounding modes of FPCR.RMode
// plus ties-away and round-to-odd, the cumulative exception flags of FPSR,
// and the reciprocal (square root) estimates.
//
// Addition, multiplication, division, square root and fused multiply-add
// use the host's IEEE 754 arithmetic with its rounding mode and flags set
// from FPCR; everything around them (NaNs, flushing, underflow before
// rounding) is done here. Conversions, rounding to integral and the
// estimates are done in integer arithmetic. Formats: ft 0 single, 1 double,
// 3 half.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#if !(defined(__x86_64__) || defined(__i386__))
#include <cfenv>
#endif

namespace vp {
namespace a64 {

class FpCore {
public:
    enum : uint32_t { IOC = 1, DZC = 2, OFC = 4, UFC = 8, IXC = 16, IDC = 128 };
    // rounding: 0 nearest even, 1 towards +inf, 2 towards -inf, 3 towards zero,
    // 4 nearest with ties away, 5 to odd (FPCR.RMode uses 0 to 3)
    FpCore(uint32_t fpcr, uint32_t& fpsr) : fpcr_(fpcr), fpsr_(fpsr) {}

    unsigned rmode() const { return (fpcr_ >> 22) & 3; }
    bool fz() const { return fpcr_ >> 24 & 1; }
    bool dn() const { return fpcr_ >> 25 & 1; }
    bool ahp() const { return fpcr_ >> 26 & 1; }

    // ------------------------------------------------------------ format
    static unsigned width(unsigned ft) { return ft == 0 ? 32 : ft == 1 ? 64 : 16; }
    static unsigned fbits(unsigned ft) { return ft == 0 ? 23 : ft == 1 ? 52 : 10; }
    static unsigned ebits(unsigned ft) { return ft == 0 ? 8 : ft == 1 ? 11 : 5; }
    static uint64_t signBit(unsigned ft) { return 1ull << (width(ft) - 1); }
    static uint64_t fracMask(unsigned ft) { return (1ull << fbits(ft)) - 1; }
    static uint64_t expMask(unsigned ft) { return ((1ull << ebits(ft)) - 1) << fbits(ft); }
    static uint64_t quietBit(unsigned ft) { return 1ull << (fbits(ft) - 1); }
    static uint64_t defNaN(unsigned ft) { return expMask(ft) | quietBit(ft); }
    static bool isNaN(uint64_t a, unsigned ft) { return (a & expMask(ft)) == expMask(ft) && (a & fracMask(ft)); }
    static bool isSNaN(uint64_t a, unsigned ft) { return isNaN(a, ft) && !(a & quietBit(ft)); }
    static bool isInf(uint64_t a, unsigned ft) { return (a & ~signBit(ft)) == expMask(ft); }
    static bool isZero(uint64_t a, unsigned ft) { return !(a & ~signBit(ft)); }
    static bool isDenorm(uint64_t a, unsigned ft) { return !(a & expMask(ft)) && (a & fracMask(ft)); }
    static uint64_t inf(unsigned ft, bool neg) { return expMask(ft) | (neg ? signBit(ft) : 0); }
    static uint64_t maxNormal(unsigned ft, bool neg) { return (expMask(ft) - (1ull << fbits(ft))) | fracMask(ft) | (neg ? signBit(ft) : 0); }
    static uint64_t minNormal(unsigned ft) { return 1ull << fbits(ft); }
    static uint64_t one(unsigned ft) { return ((1ull << (ebits(ft) - 1)) - 1) << fbits(ft); }
    static double toDouble(uint64_t a, unsigned ft) {
        if (ft == 1) { double d; memcpy(&d, &a, 8); return d; }
        if (ft == 0) { float f; uint32_t u = (uint32_t)a; memcpy(&f, &u, 4); return f; }
        const int e = (int)(a >> 10 & 31), m = (int)(a & 1023);
        const double v = e ? std::ldexp(m | 1024, e - 25) : std::ldexp(m, -24);
        return a >> 15 ? -v : v;
    }

    // Input denormals become zero under FPCR.FZ (single and double only).
    uint64_t flushIn(uint64_t a, unsigned ft) {
        if (ft != 3 && fz() && isDenorm(a, ft)) { fpsr_ |= IDC; return a & signBit(ft); }
        return a;
    }
    uint64_t procNaN(uint64_t a, unsigned ft) {
        if (isSNaN(a, ft)) fpsr_ |= IOC;
        return dn() ? defNaN(ft) : (a | quietBit(ft));
    }
    bool nans2(uint64_t a, uint64_t b, unsigned ft, uint64_t& r) {
        if (isSNaN(a, ft)) r = procNaN(a, ft);
        else if (isSNaN(b, ft)) r = procNaN(b, ft);
        else if (isNaN(a, ft)) r = procNaN(a, ft);
        else if (isNaN(b, ft)) r = procNaN(b, ft);
        else return false;
        return true;
    }
    bool nans3(uint64_t a, uint64_t b, uint64_t c, unsigned ft, uint64_t& r) {
        if (isSNaN(a, ft)) r = procNaN(a, ft);
        else if (isSNaN(b, ft)) r = procNaN(b, ft);
        else if (isSNaN(c, ft)) r = procNaN(c, ft);
        else if (isNaN(a, ft)) r = procNaN(a, ft);
        else if (isNaN(b, ft)) r = procNaN(b, ft);
        else if (isNaN(c, ft)) r = procNaN(c, ft);
        else return false;
        return true;
    }

    // ------------------------------------------------------------ arithmetic
    uint64_t add(uint64_t a, uint64_t b, unsigned ft) { return arith2(a, b, ft, 0); }
    uint64_t sub(uint64_t a, uint64_t b, unsigned ft) { return arith2(a, b, ft, 1); }
    uint64_t mul(uint64_t a, uint64_t b, unsigned ft) { return arith2(a, b, ft, 2); }
    uint64_t div(uint64_t a, uint64_t b, unsigned ft) { return arith2(a, b, ft, 3); }
    uint64_t mulx(uint64_t a, uint64_t b, unsigned ft) {  // FMULX: inf x 0 is 2
        a = flushIn(a, ft), b = flushIn(b, ft);
        uint64_t r;
        if (nans2(a, b, ft, r)) return r;
        if ((isInf(a, ft) && isZero(b, ft)) || (isZero(a, ft) && isInf(b, ft)))
            return (one(ft) + (1ull << fbits(ft))) | ((a ^ b) & signBit(ft));
        return host(ft, 2, a, b, 0);
    }
    uint64_t sqrt(uint64_t a, unsigned ft) {
        a = flushIn(a, ft);
        if (isNaN(a, ft)) return procNaN(a, ft);
        return host(ft, 4, a, 0, 0);
    }
    // addend + n * m, with one rounding (FPMulAdd); negate n or the addend
    // before the call for FMLS, FMSUB, FNMADD, FNMSUB
    uint64_t fma(uint64_t addend, uint64_t n, uint64_t m, unsigned ft) {
        addend = flushIn(addend, ft), n = flushIn(n, ft), m = flushIn(m, ft);
        const bool infZero = (isInf(n, ft) && isZero(m, ft)) || (isZero(n, ft) && isInf(m, ft));
        uint64_t r;
        if (nans3(addend, n, m, ft, r)) {
            if (isNaN(addend, ft) && !isSNaN(addend, ft) && infZero) { fpsr_ |= IOC; return defNaN(ft); }
            return r;
        }
        return host(ft, 5, n, m, addend);
    }
    uint64_t neg(uint64_t a, unsigned ft) const { return a ^ signBit(ft); }
    uint64_t abs(uint64_t a, unsigned ft) const { return a & ~signBit(ft); }
    uint64_t abd(uint64_t a, uint64_t b, unsigned ft) { return abs(sub(a, b, ft), ft); }
    // FMAX, FMIN (nm: FMAXNM, FMINNM: a quiet NaN against a number gives the number)
    uint64_t max(uint64_t a, uint64_t b, unsigned ft, bool nm) { return minmax(a, b, ft, nm, true); }
    uint64_t min(uint64_t a, uint64_t b, unsigned ft, bool nm) { return minmax(a, b, ft, nm, false); }
    // FRECPS: 2 - a * b; FRSQRTS: (3 - a * b) / 2 (fused)
    uint64_t recps(uint64_t a, uint64_t b, unsigned ft) { return step(a, b, ft, false); }
    uint64_t rsqrts(uint64_t a, uint64_t b, unsigned ft) { return step(a, b, ft, true); }

    // FCMP, FCMPE: NZCV in bits 31:28
    uint32_t compare(uint64_t a, uint64_t b, unsigned ft, bool signaling) {
        a = flushIn(a, ft), b = flushIn(b, ft);
        if (isNaN(a, ft) || isNaN(b, ft)) {
            if (signaling || isSNaN(a, ft) || isSNaN(b, ft)) fpsr_ |= IOC;
            return 0x30000000;
        }
        const double x = toDouble(a, ft), y = toDouble(b, ft);
        return x == y ? 0x60000000 : x < y ? 0x80000000 : 0x20000000;
    }
    // vector compares: kind 0 EQ (quiet), 1 GE, 2 GT (signaling)
    bool cmp(uint64_t a, uint64_t b, unsigned ft, unsigned kind) {
        a = flushIn(a, ft), b = flushIn(b, ft);
        if (isNaN(a, ft) || isNaN(b, ft)) {
            if (kind || isSNaN(a, ft) || isSNaN(b, ft)) fpsr_ |= IOC;
            return false;
        }
        const double x = toDouble(a, ft), y = toDouble(b, ft);
        return kind == 0 ? x == y : kind == 1 ? x >= y : x > y;
    }

    // ------------------------------------------------------------ conversions
    // between formats (FCVT, FCVTL, FCVTN; rnd 5 for FCVTXN)
    uint64_t convert(uint64_t a, unsigned from, unsigned to, int rnd = -1) {
        const unsigned r = rnd < 0 ? rmode() : (unsigned)rnd;
        const bool altFrom = from == 3 && ahp(), altTo = to == 3 && ahp();
        if (from != 3) a = flushIn(a, from);
        const bool sign = a & signBit(from);
        if (!altFrom && isNaN(a, from)) {
            if (isSNaN(a, from)) fpsr_ |= IOC;
            if (altTo) { fpsr_ |= IOC; return sign ? signBit(to) : 0; }
            if (dn()) return defNaN(to);
            // keep the sign and the top fraction bits
            const uint64_t frac = a & fracMask(from);
            const uint64_t f = fbits(from) >= fbits(to) ? frac >> (fbits(from) - fbits(to)) : frac << (fbits(to) - fbits(from));
            return (sign ? signBit(to) : 0) | expMask(to) | quietBit(to) | (f & fracMask(to));
        }
        if (!altFrom && isInf(a, from)) {
            if (altTo) { fpsr_ |= IOC; return (sign ? signBit(to) : 0) | (signBit(to) - 1); }
            return inf(to, sign);
        }
        if (isZero(a, from)) return sign ? signBit(to) : 0;
        int e;
        const uint64_t m = mantissa(a, from, e);
        return pack(sign, m, e, to, r, to != 3, altTo);
    }
    // to an integer of `bits` bits (FCVT*S, FCVT*U), with `fb` fraction bits
    uint64_t toInt(uint64_t a, unsigned ft, unsigned bits, bool isSigned, unsigned r, unsigned fb = 0) {
        a = flushIn(a, ft);
        const uint64_t lim = isSigned ? (1ull << (bits - 1)) - 1 : (bits == 64 ? ~0ull : (1ull << bits) - 1);
        const uint64_t mk = bits == 64 ? ~0ull : (1ull << bits) - 1;
        if (isNaN(a, ft)) { fpsr_ |= IOC; return 0; }
        const bool sign = a & signBit(ft);
        if (isInf(a, ft)) { fpsr_ |= IOC; return sign ? (isSigned ? (lim + 1) & mk : 0) : lim; }
        if (isZero(a, ft)) return 0;
        int e;
        uint64_t m = mantissa(a, ft, e);
        e += (int)fb;
        // integer part and the rest: value = m * 2^e
        unsigned __int128 ip;
        bool half = false, sticky = false;
        if (e >= 0) {
            if (e > 70) { fpsr_ |= IOC; return sign ? (isSigned ? (lim + 1) & mk : 0) : lim; }
            ip = (unsigned __int128)m << e;
        } else {
            const unsigned sh = (unsigned)-e;
            if (sh >= 66) { ip = 0; sticky = true; }
            else {
                ip = sh >= 64 ? 0 : m >> sh;
                half = sh == 65 ? false : (sh == 64 ? m >> 63 : (m >> (sh - 1))) & 1;
                sticky = sh == 65 ? m != 0 : sh >= 64 ? (m << 1) != 0 : (m & ((1ull << (sh - 1)) - 1)) != 0;
            }
        }
        const bool inexact = half || sticky;
        if (roundUp(r, sign, (uint64_t)ip & 1, half, sticky)) ip++;
        // range
        bool over;
        uint64_t res;
        if (isSigned) {
            const unsigned __int128 maxMag = sign ? (unsigned __int128)lim + 1 : lim;
            over = ip > maxMag;
            res = over ? (sign ? (lim + 1) & mk : lim) : (uint64_t)(sign ? (uint64_t)(-(__int128)ip) : (uint64_t)ip) & mk;
        } else {
            over = sign ? ip != 0 : ip > lim;
            res = over ? (sign ? 0 : lim) : (uint64_t)ip;
        }
        if (over) fpsr_ |= IOC;
        else if (inexact) fpsr_ |= IXC;
        return res;
    }
    // from an integer of `bits` bits, with `fb` fraction bits (SCVTF, UCVTF)
    uint64_t fromInt(uint64_t x, unsigned bits, bool isSigned, unsigned ft, unsigned fb = 0) {
        if (bits < 64) x &= (1ull << bits) - 1;
        bool sign = false;
        if (isSigned && (x >> (bits - 1) & 1)) { sign = true; x = 0 - (x | (bits == 64 ? 0 : ~0ull << bits)); }
        if (!x) return 0;
        return pack(sign, x, -(int)fb, ft, rmode(), true, false);
    }
    // FRINT*: to an integral value; exact: FRINTX signals inexact
    uint64_t roundInt(uint64_t a, unsigned ft, unsigned r, bool exact) {
        a = flushIn(a, ft);
        if (isNaN(a, ft)) return procNaN(a, ft);
        if (isInf(a, ft) || isZero(a, ft)) return a;
        const bool sign = a & signBit(ft);
        int e;
        const uint64_t m = mantissa(a, ft, e);
        if (e >= 0) return a;  // already integral
        const unsigned sh = (unsigned)-e;
        uint64_t ip = sh >= 64 ? 0 : m >> sh;
        const bool half = sh > 64 ? false : (sh == 64 ? m >> 63 : m >> (sh - 1)) & 1;
        const bool sticky = sh > 64 ? m != 0 : sh == 64 ? (m << 1) != 0 : (m & ((1ull << (sh - 1)) - 1)) != 0;
        if (exact && (half || sticky)) fpsr_ |= IXC;
        if (roundUp(r, sign, ip & 1, half, sticky)) ip++;
        if (!ip) return sign ? signBit(ft) : 0;
        uint32_t keep = fpsr_;
        const uint64_t res = pack(sign, ip, 0, ft, 3, false, false);
        fpsr_ = keep;
        return res;
    }

    // ------------------------------------------------------------ estimates
    uint64_t recipEstimate(uint64_t a, unsigned ft) {  // FRECPE
        a = flushIn(a, ft);
        const bool sign = a & signBit(ft);
        if (isNaN(a, ft)) return procNaN(a, ft);
        if (isInf(a, ft)) return sign ? signBit(ft) : 0;
        if (isZero(a, ft)) { fpsr_ |= DZC; return inf(ft, sign); }
        const uint64_t mag = a & ~signBit(ft);
        // |a| < 2^-(2^(E-1)): the result overflows
        const uint64_t tiny = ft == 0 ? 0x00200000ull : 0x0004000000000000ull;  // 2^-128, 2^-1024
        if (mag < tiny) {
            fpsr_ |= OFC | IXC;
            const unsigned r = rmode();
            const bool toInf = r == 0 || (r == 1 && !sign) || (r == 2 && sign);
            return toInf ? inf(ft, sign) : maxNormal(ft, sign);
        }
        if (fz() && ((ft == 0 && mag >= 0x7e800000ull) || (ft == 1 && mag >= 0x7fd0000000000000ull))) {  // >= 2^126, 2^1022
            fpsr_ |= UFC;
            return sign ? signBit(ft) : 0;
        }
        uint64_t frac = ft == 0 ? (a & 0x7fffff) << 29 : a & 0xfffffffffffffull;
        int exp = (int)((a & expMask(ft)) >> fbits(ft));
        if (exp == 0) {
            if (!(frac >> 51 & 1)) { exp = -1; frac = (frac << 2) & 0xfffffffffffffull; }
            else frac = (frac << 1) & 0xfffffffffffffull;
        }
        const unsigned scaled = (unsigned)(0x100 | (frac >> 44));
        int rexp = (ft == 0 ? 253 : 2045) - exp;
        const unsigned est = recipEst(scaled);
        frac = (uint64_t)(est & 0xff) << 44;
        if (rexp == 0) frac = (1ull << 51) | (frac >> 1);
        else if (rexp == -1) { frac = (1ull << 50) | (frac >> 2); rexp = 0; }
        if (ft == 0) return (sign ? 0x80000000ull : 0) | (uint64_t)(rexp & 0xff) << 23 | (frac >> 29);
        return (sign ? signBit(1) : 0) | (uint64_t)(rexp & 0x7ff) << 52 | frac;
    }
    uint64_t rsqrtEstimate(uint64_t a, unsigned ft) {  // FRSQRTE
        a = flushIn(a, ft);
        const bool sign = a & signBit(ft);
        if (isNaN(a, ft)) return procNaN(a, ft);
        if (isZero(a, ft)) { fpsr_ |= DZC; return inf(ft, sign); }
        if (sign) { fpsr_ |= IOC; return defNaN(ft); }
        if (isInf(a, ft)) return 0;
        uint64_t frac = ft == 0 ? (a & 0x7fffff) << 29 : a & 0xfffffffffffffull;
        int exp = (int)((a & expMask(ft)) >> fbits(ft));
        if (exp == 0) {
            while (!(frac >> 51 & 1)) { frac = (frac << 1) & 0xfffffffffffffull; exp--; }
            frac = (frac << 1) & 0xfffffffffffffull;
        }
        const unsigned scaled = (exp & 1) == 0 ? (unsigned)(0x100 | (frac >> 44)) : (unsigned)(0x80 | (frac >> 45));
        const int num = (ft == 0 ? 380 : 3068) - exp;
        const int rexp = num >= 0 ? num / 2 : -((-num + 1) / 2);
        const unsigned est = rsqrtEst(scaled);
        if (ft == 0) return (uint64_t)(rexp & 0xff) << 23 | (uint64_t)(est & 0xff) << 15;
        return (uint64_t)(rexp & 0x7ff) << 52 | (uint64_t)(est & 0xff) << 44;
    }
    uint64_t recpX(uint64_t a, unsigned ft) {  // FRECPX
        a = flushIn(a, ft);
        if (isNaN(a, ft)) return procNaN(a, ft);
        const uint64_t s = a & signBit(ft), e = (a & expMask(ft)) >> fbits(ft), emax = (1ull << ebits(ft)) - 1;
        if (e == 0) return s | (emax - 1) << fbits(ft);
        return s | ((~e & emax) << fbits(ft));
    }
    static uint32_t urecpe(uint32_t a) { return a >> 31 ? recipEst(a >> 23) << 23 : 0xffffffffu; }
    static uint32_t ursqrte(uint32_t a) { return a >> 30 ? rsqrtEst(a >> 23) << 23 : 0xffffffffu; }

private:
    uint32_t fpcr_;
    uint32_t& fpsr_;

    static unsigned recipEst(unsigned a) {  // RecipEstimate: 9 bits in [256, 511]
        a = a * 2 + 1;
        const unsigned b = (1u << 19) / a;
        return (b + 1) / 2;
    }
    static unsigned rsqrtEst(unsigned a) {  // RecipSqrtEstimate: [128, 511] -> [256, 511]
        if (a < 256) a = a * 2 + 1;
        else { a = (a >> 1) << 1; a = (a + 1) * 2; }
        unsigned b = 512;
        while ((uint64_t)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
        return (b + 1) / 2;
    }
    static bool roundUp(unsigned r, bool sign, bool lsb, bool half, bool sticky) {
        switch (r) {
            case 0: return half && (sticky || lsb);
            case 1: return (half || sticky) && !sign;
            case 2: return (half || sticky) && sign;
            case 4: return half;
            default: return false;  // towards zero; odd is handled by the caller
        }
    }
    // a finite non-zero value as m * 2^e
    static uint64_t mantissa(uint64_t a, unsigned ft, int& e) {
        const uint64_t ex = (a & expMask(ft)) >> fbits(ft), fr = a & fracMask(ft);
        const int bias = (1 << (ebits(ft) - 1)) - 1;
        if (ex == 0) { e = 1 - bias - (int)fbits(ft); return fr; }
        e = (int)ex - bias - (int)fbits(ft);
        return fr | (1ull << fbits(ft));
    }
    // round sign * m * 2^e into format ft (FPRoundBase); fzOut: FPCR.FZ
    // applies; alt: the alternative half-precision format
    uint64_t pack(bool sign, uint64_t m, int e, unsigned ft, unsigned r, bool fzOut, bool alt) {
        const unsigned F = fbits(ft), E = ebits(ft);
        const int bias = (1 << (E - 1)) - 1, minExp = 1 - bias;
        const uint64_t sgn = sign ? signBit(ft) : 0;
        const int lz = __builtin_clzll(m);
        m <<= lz;
        const int exponent = e + 63 - lz;  // value = 1.f * 2^exponent
        if (fzOut && fz() && ft != 3 && exponent < minExp) { fpsr_ |= UFC; return sgn; }
        const int unitExp = (exponent > minExp ? exponent : minExp) - (int)F;
        const int sh = unitExp - (exponent - 63);  // > 0
        uint64_t im;
        bool half, sticky;
        if (sh >= 65) { im = 0; half = false; sticky = true; }
        else if (sh == 64) { im = 0; half = m >> 63; sticky = (m << 1) != 0; }
        else { im = m >> sh; half = m >> (sh - 1) & 1; sticky = sh > 1 && (m & ((1ull << (sh - 1)) - 1)); }
        const bool inexact = half || sticky;
        if (exponent < minExp && inexact) fpsr_ |= UFC;
        if (r == 5) { if (inexact) im |= 1; }
        else if (roundUp(r, sign, im & 1, half, sticky)) im++;
        int uexp = unitExp;
        if (im >> (F + 1)) { im >>= 1; uexp++; }
        uint64_t biased, frac;
        if (im >> F) { biased = (uint64_t)(uexp + (int)F - minExp + 1); frac = im & fracMask(ft); }
        else { biased = 0; frac = im; }
        const uint64_t emax = alt ? (1ull << E) : (1ull << E) - 1;
        if (biased >= emax) {
            if (alt) { fpsr_ |= IOC; return sgn | (signBit(ft) - 1); }
            fpsr_ |= OFC | IXC;
            const bool toInf = r == 0 || r == 4 || (r == 1 && !sign) || (r == 2 && sign);
            return toInf ? inf(ft, sign) : maxNormal(ft, sign);
        }
        if (inexact) fpsr_ |= IXC;
        return sgn | biased << F | frac;
    }
    uint64_t arith2(uint64_t a, uint64_t b, unsigned ft, int op) {
        a = flushIn(a, ft), b = flushIn(b, ft);
        uint64_t r;
        if (nans2(a, b, ft, r)) return r;
        return host(ft, op, a, b, 0);
    }
    uint64_t minmax(uint64_t a, uint64_t b, unsigned ft, bool nm, bool isMax) {
        a = flushIn(a, ft), b = flushIn(b, ft);
        if (nm) {
            const bool qa = isNaN(a, ft) && !isSNaN(a, ft), qb = isNaN(b, ft) && !isSNaN(b, ft);
            if (qa && !isNaN(b, ft)) a = isMax ? inf(ft, true) : inf(ft, false);
            else if (qb && !isNaN(a, ft)) b = isMax ? inf(ft, true) : inf(ft, false);
        }
        uint64_t r;
        if (nans2(a, b, ft, r)) return r;
        const double x = toDouble(a, ft), y = toDouble(b, ft);
        if (x == y) {  // equal, or zeros of either sign
            if (isZero(a, ft) && isZero(b, ft)) return isMax ? (a & b) : (a | b);
            return a;
        }
        return (x > y) == isMax ? a : b;
    }
    uint64_t step(uint64_t a, uint64_t b, unsigned ft, bool sqrtStep) {
        a = neg(a, ft);
        a = flushIn(a, ft), b = flushIn(b, ft);
        uint64_t r;
        if (nans2(a, b, ft, r)) return r;
        const uint64_t two = one(ft) + (1ull << fbits(ft));                       // 2.0
        const uint64_t three = two | (1ull << (fbits(ft) - 1));                  // 3.0
        const uint64_t onePointFive = one(ft) | (1ull << (fbits(ft) - 1));       // 1.5
        if ((isInf(a, ft) && isZero(b, ft)) || (isZero(a, ft) && isInf(b, ft))) return sqrtStep ? onePointFive : two;
        if (isInf(a, ft) || isInf(b, ft)) return inf(ft, (a ^ b) & signBit(ft));
        if (!sqrtStep) return host(ft, 5, a, b, two);
        return host(ft, 6, a, b, three);
    }

    // The host's arithmetic: op 0 add, 1 sub, 2 mul, 3 div, 4 sqrt,
    // 5 fma (a * b + c), 6 (a * b + c) / 2. Inputs are not NaNs.
    uint64_t host(unsigned ft, int op, uint64_t a, uint64_t b, uint64_t c) {
        unsigned ex;
        uint64_t r = hostOp(ft, op, a, b, c, rmode(), ex);
        if (isNaN(r, ft)) r = defNaN(ft);  // an invalid operation
        const bool inexact = ex & 0x20;
        const uint64_t mag = r & ~signBit(ft);
        // Arm detects tininess before rounding: a result that rounded up to
        // the smallest normal may have been tiny
        const bool tiny = (ex & 0x10) || (mag < minNormal(ft) && (mag || inexact)) ||
                          (mag == minNormal(ft) && inexact && rmode() != 3 && [&] {
                              unsigned ex2;
                              return (hostOp(ft, op, a, b, c, 3, ex2) & ~signBit(ft)) < minNormal(ft);
                          }());
        if (fz() && tiny) { fpsr_ |= UFC; return r & signBit(ft); }
        uint32_t f = 0;
        if (ex & 1) f |= IOC;
        if (ex & 4) f |= DZC;
        if (ex & 8) f |= OFC;
        if (tiny && inexact) f |= UFC;
        if (inexact) f |= IXC;
        fpsr_ |= f;
        return r;
    }
    // MXCSR flags: 1 invalid, 4 divide by zero, 8 overflow, 0x10 underflow, 0x20 inexact
    static uint64_t hostOp(unsigned ft, int op, uint64_t a, uint64_t b, uint64_t c, unsigned r, unsigned& ex) {
        uint64_t res;
#if defined(__x86_64__) || defined(__i386__)
        static const unsigned rc[4] = {0u << 13, 2u << 13, 1u << 13, 3u << 13};
        unsigned csr = 0x1f80 | rc[r], old, after;
        __asm__ volatile("stmxcsr %0" : "=m"(old));
        __asm__ volatile("ldmxcsr %0" ::"m"(csr));
#define VP_PIN(v) __asm__ volatile("" : "+x"(v))
#else
        const int modes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
        const int old = fegetround();
        fesetround(modes[r]);
        feclearexcept(FE_ALL_EXCEPT);
#define VP_PIN(v) __asm__ volatile("" : "+w"(v))
#endif
        if (ft == 0) {
            float x, y, z, o;
            uint32_t u;
            u = (uint32_t)a; memcpy(&x, &u, 4);
            u = (uint32_t)b; memcpy(&y, &u, 4);
            u = (uint32_t)c; memcpy(&z, &u, 4);
            VP_PIN(x); VP_PIN(y); VP_PIN(z);
            switch (op) {
                case 0: o = x + y; break;
                case 1: o = x - y; break;
                case 2: o = x * y; break;
                case 3: o = x / y; break;
                case 4: o = __builtin_sqrtf(x); break;
                case 5: o = __builtin_fmaf(x, y, z); break;
                default:  // (x * y + z) / 2 with one rounding: halve x when that is exact
                    if (__builtin_fabsf(x) >= 2 * __FLT_MIN__) { x = x * 0.5f; z = z * 0.5f; VP_PIN(x); VP_PIN(z); o = __builtin_fmaf(x, y, z); }
                    else { o = __builtin_fmaf(x, y, z); VP_PIN(o); o = o * 0.5f; }
                    break;
            }
            VP_PIN(o);
            memcpy(&u, &o, 4);
            res = u;
        } else {
            double x, y, z, o;
            memcpy(&x, &a, 8); memcpy(&y, &b, 8); memcpy(&z, &c, 8);
            VP_PIN(x); VP_PIN(y); VP_PIN(z);
            switch (op) {
                case 0: o = x + y; break;
                case 1: o = x - y; break;
                case 2: o = x * y; break;
                case 3: o = x / y; break;
                case 4: o = __builtin_sqrt(x); break;
                case 5: o = __builtin_fma(x, y, z); break;
                default:
                    if (__builtin_fabs(x) >= 2 * __DBL_MIN__) { x = x * 0.5; z = z * 0.5; VP_PIN(x); VP_PIN(z); o = __builtin_fma(x, y, z); }
                    else { o = __builtin_fma(x, y, z); VP_PIN(o); o = o * 0.5; }
                    break;
            }
            VP_PIN(o);
            memcpy(&res, &o, 8);
        }
#undef VP_PIN
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("stmxcsr %0" : "=m"(after));
        __asm__ volatile("ldmxcsr %0" ::"m"(old));
        ex = after & 0x3d;
#else
        const int fl = fetestexcept(FE_ALL_EXCEPT);
        ex = (fl & FE_INVALID ? 1 : 0) | (fl & FE_DIVBYZERO ? 4 : 0) | (fl & FE_OVERFLOW ? 8 : 0) | (fl & FE_UNDERFLOW ? 0x10 : 0) | (fl & FE_INEXACT ? 0x20 : 0);
        fesetround(old);
#endif
        return res;
    }
};

}  // namespace a64
}  // namespace vp
