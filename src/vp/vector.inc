// vector.inc - the V extension (RISC-V "V" Standard Extension for Vector
// Operations 1.0), included in the body of class Machine.
//
// VLEN is the hart's vlen (128 bits unless the platform says otherwise, up
// to Hart::VLEN_MAX), ELEN 64. The registers are one array, so a register
// group is contiguous bytes. Every tail and inactive element is left
// undisturbed, which is what "agnostic" allows too. Vector floating point is
// single and double (SEW 32 and 64), with the scalar FPU's arithmetic, flags
// and canonical NaNs. A memory access faulting at element i leaves vstart at
// i and traps with that element's address; fault-only-first loads trim vl
// instead past element 0.
//
// vexec returns VOK, VILL (illegal instruction), VTRAP (cause and tval set)
// or VPEND (the access must wait for the ordered phase).
enum { VOK = 0, VILL = 1, VTRAP = 2, VPEND = 3 };

static bool vtypeDecode(uint64_t vt, bool rv64, VType& t) {
    const uint64_t reserved = vt >> 8;
    if (reserved & (rv64 ? ~0ull >> 8 : 0xffffffull)) return false;
    const unsigned vsew = (vt >> 3) & 7, vlmul = vt & 7;
    if (vsew > 3 || vlmul == 4) return false;
    t.sew = 8u << vsew;
    t.lmul = vlmul < 4 ? (int)vlmul : (int)vlmul - 8;
    t.ta = (vt >> 6) & 1;
    t.ma = (vt >> 7) & 1;
    if (t.lmul < 0 && t.sew > (64u >> -t.lmul)) return false;  // SEW <= LMUL * ELEN
    return true;
}
static uint64_t vlmaxOf(unsigned vlen, const VType& t) {
    return t.lmul >= 0 ? ((uint64_t)vlen / t.sew) << t.lmul : ((uint64_t)vlen / t.sew) >> -t.lmul;
}

static uint64_t vsext(uint64_t v, unsigned bits) {
    return bits >= 64 ? v : (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
}
static uint64_t vmaskw(unsigned bits) { return bits >= 64 ? ~0ull : (1ull << bits) - 1; }

// Element i of width eew bytes in the group starting at register r.
uint8_t* vptr(Hart& h, unsigned r, uint64_t i, unsigned eewB) {
    return h.vr + (size_t)r * (h.vlen / 8) + i * eewB;
}
uint64_t vget(Hart& h, unsigned r, uint64_t i, unsigned eewB) {
    uint64_t v = 0;
    memcpy(&v, vptr(h, r, i, eewB), eewB);
    return v;
}
void vset(Hart& h, unsigned r, uint64_t i, unsigned eewB, uint64_t v) { memcpy(vptr(h, r, i, eewB), &v, eewB); }
bool vbit(Hart& h, unsigned r, uint64_t i) { return (h.vr[(size_t)r * (h.vlen / 8) + i / 8] >> (i & 7)) & 1; }
void vsetbit(Hart& h, unsigned r, uint64_t i, bool b) {
    uint8_t& c = h.vr[(size_t)r * (h.vlen / 8) + i / 8];
    c = (uint8_t)((c & ~(1u << (i & 7))) | ((b ? 1u : 0u) << (i & 7)));
}

// Fixed-point rounding (vxrm) of v >> d: the bits shifted out decide.
static uint64_t vround(uint64_t v, unsigned d, unsigned vxrm) {
    if (d == 0) return 0;
    const uint64_t bit = (v >> (d - 1)) & 1;
    const uint64_t rest = d >= 2 ? v & ((d - 1 >= 64) ? ~0ull : (1ull << (d - 1)) - 1) : 0;
    const uint64_t lsb = d >= 64 ? 0 : (v >> d) & 1;
    switch (vxrm & 3) {
        case 0: return bit;                          // rnu
        case 1: return bit & ((rest != 0) | lsb);    // rne
        case 2: return 0;                            // rdn
        default: return !lsb && (bit | (rest != 0)); // rod
    }
}

// vfrec7 and vfrsqrt7: the 7-bit estimates the specification tabulates.
static const uint8_t* vrec7Table() {
    static uint8_t t[128];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 128; i++) {
            long double x = 1.0L + (i + 0.5L) / 128.0L;
            long double r = 2.0L / x - 1.0L;
            int v = (int)std::floor(r * 128.0L + 0.5L);
            t[i] = (uint8_t)(v > 127 ? 127 : v);
        }
        init = true;
    }
    return t;
}
static const uint8_t* vrsqrt7Table() {
    static uint8_t t[128];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 128; i++) {
            // index: the biased exponent's low bit, then 6 significand bits;
            // an odd biased exponent is an even power of two
            long double sig = 1.0L + ((i & 63) + 0.5L) / 64.0L;
            long double x = (i & 64) ? sig : sig * 2.0L;
            long double y = 1.0L / std::sqrt(x);           // normalized to [1,2) below
            while (y < 1.0L) y *= 2.0L;
            int v = (int)std::floor((y - 1.0L) * 128.0L + 0.5L);
            t[i] = (uint8_t)(v > 127 ? 127 : v);
        }
        init = true;
    }
    return t;
}

// vfrec7.v on one element of `bits` (32 or 64).
static uint64_t vfrec7(uint64_t u, unsigned bits, unsigned rm, uint32_t& fl) {
    const unsigned eb = bits == 32 ? 8 : 11, mb = bits == 32 ? 23 : 52;
    const int bias = (1 << (eb - 1)) - 1;
    const uint64_t sign = u >> (bits - 1) & 1;
    int64_t e = (int64_t)((u >> mb) & ((1ull << eb) - 1));
    uint64_t m = u & ((1ull << mb) - 1);
    const uint64_t sbit = sign << (bits - 1), emax = (1ull << eb) - 1;
    const uint64_t qnan = bits == 32 ? fpu::QNAN32 : fpu::QNAN64;
    if (e == (int64_t)emax) {
        if (m) { if (!(m >> (mb - 1))) fl |= fpu::NV; return qnan; }  // NaN
        return sbit;                                                    // +-inf -> +-0
    }
    if (e == 0 && m == 0) { fl |= fpu::DZ; return sbit | (emax << mb); }  // +-0 -> +-inf
    if (e == 0) {                       // subnormal: normalize, e = -(leading zeros)
        while (!(m >> (mb - 1))) { m <<= 1; e--; }
        m = (m << 1) & ((1ull << mb) - 1);
    }
    int64_t oe = 2 * bias - 1 - e;
    const uint64_t idx = m >> (mb - 7);
    uint64_t om = (uint64_t)vrec7Table()[idx] << (mb - 7);
    if (oe <= 0) {                      // a subnormal result
        om = (om | (1ull << mb)) >> (1 - oe);
        oe = 0;
    }
    if (oe >= (int64_t)emax) {          // the input was a small subnormal: overflow
        fl |= fpu::OF | fpu::NX;
        bool toInf = rm == fpu::RNE || rm == fpu::RMM || (rm == fpu::RUP && !sign) || (rm == fpu::RDN && sign);
        return toInf ? sbit | (emax << mb) : sbit | ((emax - 1) << mb) | ((1ull << mb) - 1);
    }
    return sbit | ((uint64_t)oe << mb) | om;
}
// vfrsqrt7.v on one element.
static uint64_t vfrsqrt7(uint64_t u, unsigned bits, uint32_t& fl) {
    const unsigned eb = bits == 32 ? 8 : 11, mb = bits == 32 ? 23 : 52;
    const int bias = (1 << (eb - 1)) - 1;
    const uint64_t sign = u >> (bits - 1) & 1;
    int64_t e = (int64_t)((u >> mb) & ((1ull << eb) - 1));
    uint64_t m = u & ((1ull << mb) - 1);
    const uint64_t emax = (1ull << eb) - 1;
    const uint64_t qnan = bits == 32 ? fpu::QNAN32 : fpu::QNAN64;
    if (e == (int64_t)emax && m) { if (!(m >> (mb - 1))) fl |= fpu::NV; return qnan; }
    if (e == 0 && m == 0) { fl |= fpu::DZ; return (sign << (bits - 1)) | (emax << mb); }  // +-0 -> +-inf
    if (sign) { fl |= fpu::NV; return qnan; }                                              // negative
    if (e == (int64_t)emax) return 0;                                                       // +inf -> +0
    if (e == 0) {                                    // subnormal: normalize
        while (!(m >> (mb - 1))) { m <<= 1; e--; }
        m = (m << 1) & ((1ull << mb) - 1);
    }
    const uint64_t idx = ((uint64_t)(e & 1) << 6) | (m >> (mb - 6));
    const int64_t oe = (3 * bias - 1 - e) / 2;
    return ((uint64_t)oe << mb) | ((uint64_t)vrsqrt7Table()[idx] << (mb - 7));
}

int vexec(Hart& h, uint32_t insn, uint32_t& cause, uint64_t& tval, int& wrd, uint64_t& wval) {
    const unsigned op = insn & 0x7f, rd = (insn >> 7) & 31, f3 = (insn >> 12) & 7, rs1 = (insn >> 15) & 31,
                   rs2 = (insn >> 20) & 31, vm = (insn >> 25) & 1, f6 = insn >> 26;
    const bool rv64 = h.rv64;
    if (!(h.mstatus & MSTATUS_VS)) return VILL;
    const unsigned vlenb = h.vlen / 8;
    auto dirty = [&] { h.mstatus |= MSTATUS_VS; };
    auto xw = [&](unsigned r, uint64_t v) {
        v = h.sx(v);
        if (r) { h.x[r] = v; wrd = (int)r; wval = v; }
    };

    // ---------------------------------------------------- vsetvl{i}
    if (op == 0x57 && f3 == 7) {
        uint64_t vt;
        if (!(insn >> 31)) vt = (insn >> 20) & 0x7ff;                  // vsetvli
        else if ((insn >> 30) == 3) vt = (insn >> 20) & 0x3ff;         // vsetivli
        else if ((insn >> 25) == 0x40) vt = h.x[rs2];                  // vsetvl
        else return VILL;
        VType t;
        const bool ok = vtypeDecode(h.ax(vt), rv64, t);
        const uint64_t vlmax = ok ? vlmaxOf(h.vlen, t) : 0;
        uint64_t avl;
        if ((insn >> 30) == 3) avl = rs1;                              // uimm
        else if (rs1) avl = h.ax(h.x[rs1]);
        else if (rd) avl = ~0ull;
        else {                                                         // keep vl
            if (!ok || h.vill || vlmax != vlmaxOf(h.vlen, h.vt)) { h.vill = true; h.vl = 0; xw(rd, 0); dirty(); h.vstart = 0; return VOK; }
            avl = h.vl;
        }
        if (!ok) {
            h.vill = true;
            h.vl = 0;
        } else {
            h.vill = false;
            h.vt = t;
            h.vtypeRaw = h.ax(vt);
            h.vl = avl < vlmax ? avl : vlmax;
        }
        h.vstart = 0;
        xw(rd, h.vl);
        dirty();
        return VOK;
    }

    // ---------------------------------------------------- loads and stores
    if (op == 0x07 || op == 0x27) {
        const bool st = op == 0x27;
        const unsigned eewB = f3 == 0 ? 1 : f3 == 5 ? 2 : f3 == 6 ? 4 : 8;
        const unsigned nf = (insn >> 29) + 1, mop = (insn >> 26) & 3;
        if ((insn >> 28) & 1) return VILL;                             // mew
        const unsigned umop = rs2;
        // Whole registers: vl<nf>re<eew>.v, vs<nf>r.v.
        if (mop == 0 && umop == 8) {
            if (!vm || (nf & (nf - 1)) || (rd % nf) || (st && eewB != 1)) return VILL;
            if (h.par) return VPEND;
            const uint64_t evl = (uint64_t)nf * vlenb / eewB;
            const uint64_t base = h.x[rs1];
            for (uint64_t i = h.vstart; i < evl; i++) {
                const uint64_t a = h.ax(base + i * eewB);
                Acc r;
                if (st) r = store(h, a, eewB, vget(h, rd, i, eewB));
                else { uint64_t v; r = load(h, a, eewB, v); if (r == Acc::Ok) vset(h, rd, i, eewB, v); }
                if (r != Acc::Ok) {
                    if (r == Acc::Pending) return VPEND;
                    h.vstart = i;
                    cause = (st ? 7 : 5) + (r == Acc::PageFault ? 8 : 0);
                    tval = a;
                    return VTRAP;
                }
            }
            h.vstart = 0;
            if (!st) dirty();
            return VOK;
        }
        if (h.vill) return VILL;
        const VType& t = h.vt;
        const bool indexed = mop & 1;
        const bool maskLs = mop == 0 && umop == 0xb;
        const bool ff = mop == 0 && umop == 0x10;
        if (mop == 0 && umop != 0 && !maskLs && !(ff && !st)) return VILL;
        // The data's EEW and EMUL: the index's for indexed accesses are
        // eewB and its EMUL; the data is SEW and LMUL.
        const unsigned dB = indexed ? t.sew / 8 : maskLs ? 1 : eewB;
        int demul = indexed ? t.lmul : maskLs ? 0 : t.lmul + (int)__builtin_ctz(eewB * 8) - (int)__builtin_ctz(t.sew);
        if (demul > 3 || demul < -3) return VILL;
        if (maskLs && (nf != 1 || !vm || eewB != 1)) return VILL;
        const unsigned dregs = demul > 0 ? 1u << demul : 1u;
        if (nf * dregs > 8 || rd + nf * dregs > 32 || (rd % dregs)) return VILL;
        int iemul = 0;
        if (indexed) {
            iemul = t.lmul + (int)__builtin_ctz(eewB * 8) - (int)__builtin_ctz(t.sew);
            if (iemul > 3 || iemul < -3) return VILL;
            const unsigned iregs = iemul > 0 ? 1u << iemul : 1u;
            if (rs2 % iregs) return VILL;
            // A destination that overlaps the index where the specification
            // reserves it still runs: element by element, each index read
            // before its element is written.
        }
        if (!vm && !st && rd == 0) return VILL;
        if (h.par) return VPEND;
        const uint64_t evl = maskLs ? (h.vl + 7) / 8 : h.vl;
        const uint64_t base = h.x[rs1];
        const int64_t stride = mop == 2 ? (int64_t)h.sx(h.x[rs2]) : (int64_t)(nf * dB);
        for (uint64_t i = h.vstart; i < evl; i++) {
            if (!vm && !vbit(h, 0, i)) continue;
            for (unsigned f = 0; f < nf; f++) {
                uint64_t a;
                if (indexed) a = base + vget(h, rs2, i, eewB) + (uint64_t)f * dB;
                else a = base + (uint64_t)((int64_t)i * stride) + (uint64_t)f * dB;
                a = h.ax(a);
                const unsigned reg = rd + f * dregs;
                Acc r;
                if (st) r = store(h, a, dB, vget(h, reg, i, dB));
                else {
                    uint64_t v;
                    r = load(h, a, dB, v);
                    if (r == Acc::Ok) vset(h, reg, i, dB, v);
                }
                if (r != Acc::Ok) {
                    if (r == Acc::Pending) return VPEND;
                    if (ff && i > 0) {                                 // fault-only-first: trim vl
                        h.vl = i;
                        h.vstart = 0;
                        dirty();
                        return VOK;
                    }
                    h.vstart = i;
                    cause = (st ? 7 : 5) + (r == Acc::PageFault ? 8 : 0);
                    tval = a;
                    return VTRAP;
                }
            }
        }
        h.vstart = 0;
        if (!st) dirty();
        return VOK;
    }

    if (op != 0x57) return VILL;
    if (h.vill) return VILL;
    const VType& t = h.vt;
    const unsigned sew = t.sew, sB = sew / 8;
    const uint64_t vl = h.vl;
    const uint64_t smask = vmaskw(sew);
    const unsigned lregs = t.lmul > 0 ? 1u << t.lmul : 1u;
    auto aligned = [&](unsigned r, unsigned n) { return r % n == 0 && r + n <= 32; };
    auto active = [&](uint64_t i) { return vm || vbit(h, 0, i); };
    // The scalar operand: x[rs1] sign-extended to SEW; or the 5-bit immediate.
    const uint64_t simm = vsext(rs1, 5) & smask, uimm = rs1;
    const uint64_t xs = vsext(h.sx(h.x[rs1]), rv64 ? 64 : 32) & smask;

    // -------------------------------------------- floating point
    if (f3 == 1 || f3 == 5) {
        if (!(h.mstatus & MSTATUS_FS)) return VILL;
        const unsigned frm = (h.fcsr >> 5) & 7;
        if (frm > 4) return VILL;
        uint32_t fl = 0;
        const bool vf = f3 == 5;
        auto fscal = [&](unsigned bits) -> uint64_t {                  // f[rs1] at `bits`
            return bits == 32 ? fpu::unbox(h.f[rs1]) : h.f[rs1];
        };
        auto fdone = [&] {
            if (fl) { h.fcsr |= fl; h.mstatus |= MSTATUS_FS; }
            h.vstart = 0;
            dirty();
            return VOK;
        };
        auto ar = [&](fpu::Op o, uint64_t a, uint64_t b, uint64_t c, unsigned bits) -> uint64_t {
            return bits == 32 ? fpu::arith<float>(o, (uint32_t)a, (uint32_t)b, (uint32_t)c, frm, fl)
                              : fpu::arith<double>(o, a, b, c, frm, fl);
        };
        auto fmin = [&](uint64_t a, uint64_t b, bool mx, unsigned bits) -> uint64_t {
            return bits == 32 ? fpu::minmax<float>((uint32_t)a, (uint32_t)b, mx, fl) : fpu::minmax<double>(a, b, mx, fl);
        };
        auto fcmp = [&](uint64_t a, uint64_t b, unsigned kind, unsigned bits) -> uint64_t {
            return bits == 32 ? fpu::compare<float>((uint32_t)a, (uint32_t)b, kind, fl) : fpu::compare<double>(a, b, kind, fl);
        };
        const bool fpSew = sew == 32 || sew == 64;
        // Widening forms (SEW 32 only: 64 is the widest).
        const bool widening = f6 >= 0x30 || (f6 == 0x12 && rs1 >= 8 && rs1 < 16);
        // Conversions accept integer SEWs too; the rest need an FP SEW.
        if (f6 != 0x12 && !fpSew) return VILL;
        if ((widening || f6 == 0x31 || f6 == 0x33) && f6 != 0x12 && sew != 32) return VILL;
        const uint64_t fsc = vf ? fscal(sew) : 0;
        auto op1 = [&](uint64_t i) -> uint64_t { return vf ? fsc : vget(h, rs1, i, sB); };

        // Reductions: vd[0] = op over active vs2[i], starting from vs1[0].
        if (f6 == 0x01 || f6 == 0x03 || f6 == 0x05 || f6 == 0x07 || f6 == 0x31 || f6 == 0x33) {
            if (vf || h.vstart) return VILL;
            if (vl == 0) return fdone();
            const bool wide = f6 == 0x31 || f6 == 0x33;
            const unsigned ob = wide ? 64 : sew;
            uint64_t acc = vget(h, rs1, 0, ob / 8);
            for (uint64_t i = 0; i < vl; i++) {
                if (!active(i)) continue;
                uint64_t e = vget(h, rs2, i, sB);
                if (wide) e = fpu::sToD((uint32_t)e, fl);
                if (f6 == 0x05 || f6 == 0x07) acc = fmin(acc, e, f6 == 0x07, ob);
                else acc = ar(fpu::Op::Add, acc, e, 0, ob);
            }
            vset(h, rd, 0, ob / 8, acc);
            return fdone();
        }
        if (f6 == 0x10) {                                              // vfmv.f.s, vfmv.s.f
            if (!vf) {
                if (rs1 || !vm) return VILL;
                uint64_t e = vget(h, rs2, 0, sB);
                h.f[rd] = sew == 32 ? fpu::box((uint32_t)e) : e;
                h.mstatus |= MSTATUS_FS;
                wrd = 32 + (int)rd; wval = h.f[rd];
                h.vstart = 0;
                return VOK;
            }
            if (rs2 || !vm) return VILL;
            if (h.vstart < vl) vset(h, rd, 0, sB, fsc);
            return fdone();
        }
        if (f6 == 0x0e || f6 == 0x0f) {                                // vfslide1up, vfslide1down
            if (!vf || !aligned(rd, lregs) || (f6 == 0x0e && rd < rs2 + lregs && rs2 < rd + lregs)) return VILL;
            if (!vm && rd == 0) return VILL;
            for (uint64_t i = h.vstart; i < vl; i++) {
                if (!active(i)) continue;
                uint64_t v;
                if (f6 == 0x0e) v = i == 0 ? fsc : vget(h, rs2, i - 1, sB);
                else v = i + 1 < vl ? vget(h, rs2, i + 1, sB) : fsc;
                vset(h, rd, i, sB, v);
            }
            return fdone();
        }
        if (f6 == 0x17) {                                              // vfmerge.vfm, vfmv.v.f
            if (!vf || (vm && rs2) || !aligned(rd, lregs)) return VILL;
            if (!vm && rd == 0) return VILL;
            for (uint64_t i = h.vstart; i < vl; i++)
                vset(h, rd, i, sB, vm || vbit(h, 0, i) ? fsc : vget(h, rs2, i, sB));
            return fdone();
        }
        if (f6 == 0x18 || f6 == 0x19 || f6 == 0x1b || f6 == 0x1c || f6 == 0x1d || f6 == 0x1f) {  // compares
            if ((f6 == 0x1d || f6 == 0x1f) && !vf) return VILL;
            for (uint64_t i = h.vstart; i < vl; i++) {
                if (!active(i)) continue;
                uint64_t a = vget(h, rs2, i, sB), b = op1(i), r;
                switch (f6) {
                    case 0x18: r = fcmp(a, b, 2, sew); break;              // vmfeq
                    case 0x19: r = fcmp(a, b, 0, sew); break;              // vmfle
                    case 0x1b: r = fcmp(a, b, 1, sew); break;              // vmflt
                    case 0x1c: r = !fcmp(a, b, 2, sew); break;             // vmfne
                    case 0x1d: r = fcmp(b, a, 1, sew); break;              // vmfgt
                    default: r = fcmp(b, a, 0, sew); break;                // vmfge
                }
                vsetbit(h, rd, i, r);
            }
            return fdone();
        }
        if (f6 == 0x12) {                                              // VFUNARY0: conversions
            const unsigned k = rs1;
            if (vf) return VILL;
            unsigned rmode = frm;
            if (k == 6 || k == 7 || k == 14 || k == 15 || k == 22 || k == 23) rmode = fpu::RTZ;
            if (k < 8) {                                               // single width
                if (k == 4 || k == 5 || !fpSew) return VILL;
                if (!aligned(rd, lregs)) return VILL;
                if (!vm && rd == 0) return VILL;
                for (uint64_t i = h.vstart; i < vl; i++) {
                    if (!active(i)) continue;
                    uint64_t a = vget(h, rs2, i, sB), r;
                    const bool sg = k & 1;
                    if (k == 0 || k == 1 || k == 6 || k == 7)
                        r = sew == 32 ? fpu::toInt<float>((uint32_t)a, rmode, sg, 32, fl) : fpu::toInt<double>(a, rmode, sg, 64, fl);
                    else
                        r = sew == 32 ? fpu::fromInt<float>(a, k == 3, 32, rmode, fl) : fpu::fromInt<double>(a, k == 3, 64, rmode, fl);
                    vset(h, rd, i, sB, r & smask);
                }
                return fdone();
            }
            const unsigned dregs = t.lmul + 1 > 0 ? 1u << (t.lmul + 1) : 1u;
            if (k < 16) {                                              // widening: SEW -> 2*SEW
                if (k == 13 || t.lmul + 1 > 3) return VILL;
                // from a float: single only; from an integer: 16 or 32 bits
                if ((k == 10 || k == 11) ? (sew != 16 && sew != 32) : sew != 32) return VILL;
                if (!aligned(rd, dregs) || !aligned(rs2, lregs)) return VILL;
                if (rd < rs2 + lregs && rs2 < rd + dregs && !(rs2 == rd + dregs - lregs && t.lmul >= 0)) return VILL;
                if (!vm && rd == 0) return VILL;
                for (uint64_t i = h.vstart; i < vl; i++) {
                    if (!active(i)) continue;
                    uint64_t a = vget(h, rs2, i, sB), r;
                    switch (k) {
                        case 8: case 14: r = fpu::toInt<float>((uint32_t)a, rmode, false, 64, fl); break;
                        case 9: case 15: r = fpu::toInt<float>((uint32_t)a, rmode, true, 64, fl); break;
                        case 10:
                            r = sew == 16 ? fpu::fromInt<float>(a, false, 32, rmode, fl) : fpu::fromInt<double>(a, false, 32, rmode, fl);
                            break;
                        case 11:
                            r = sew == 16 ? fpu::fromInt<float>(vsext(a, 16), true, 32, rmode, fl) : fpu::fromInt<double>(a, true, 32, rmode, fl);
                            break;
                        default: r = fpu::sToD((uint32_t)a, fl); break;
                    }
                    vset(h, rd, i, 2 * sB, r);
                }
                return fdone();
            }
            // narrowing: 2*SEW -> SEW
            if (k > 23 || t.lmul + 1 > 3) return VILL;
            // to an integer: 16 or 32 bits; to a float: single only
            if ((k == 16 || k == 17 || k == 22 || k == 23) ? (sew != 16 && sew != 32) : sew != 32) return VILL;
            if (!aligned(rd, lregs) || !aligned(rs2, dregs)) return VILL;
            if (rd < rs2 + dregs && rs2 < rd + lregs && rd != rs2) return VILL;  // only the lowest part may overlap
            if (!vm && rd == 0) return VILL;
            for (uint64_t i = h.vstart; i < vl; i++) {
                if (!active(i)) continue;
                uint64_t a = vget(h, rs2, i, 2 * sB), r;
                switch (k) {
                    case 16: case 22:
                        r = sew == 16 ? fpu::toInt<float>((uint32_t)a, rmode, false, 16, fl) : fpu::toInt<double>(a, rmode, false, 32, fl);
                        break;
                    case 17: case 23:
                        r = sew == 16 ? fpu::toInt<float>((uint32_t)a, rmode, true, 16, fl) : fpu::toInt<double>(a, rmode, true, 32, fl);
                        break;
                    case 18: r = fpu::fromInt<float>(a, false, 64, rmode, fl); break;
                    case 19: r = fpu::fromInt<float>(a, true, 64, rmode, fl); break;
                    case 20: r = fpu::dToS(a, rmode, fl); break;
                    default: {                                         // vfncvt.rod.f.f.w
                        uint32_t f2 = 0;
                        r = fpu::dToS(a, fpu::RTZ, f2);
                        if ((f2 & fpu::NX) && ((r >> 23) & 0xff) != 0xff) r |= 1;
                        fl |= f2;
                        break;
                    }
                }
                vset(h, rd, i, sB, r & smask);
            }
            return fdone();
        }
        if (f6 == 0x13) {                                              // VFUNARY1
            if (vf || !aligned(rd, lregs)) return VILL;
            if (!vm && rd == 0) return VILL;
            const unsigned k = rs1;
            if (k != 0 && k != 4 && k != 5 && k != 16) return VILL;
            for (uint64_t i = h.vstart; i < vl; i++) {
                if (!active(i)) continue;
                uint64_t a = vget(h, rs2, i, sB), r;
                if (k == 0) r = ar(fpu::Op::Sqrt, a, 0, 0, sew);
                else if (k == 4) r = vfrsqrt7(a, sew, fl);
                else if (k == 5) r = vfrec7(a, sew, frm, fl);
                else r = sew == 32 ? fpu::classify<float>((uint32_t)a) : fpu::classify<double>(a);
                vset(h, rd, i, sB, r);
            }
            return fdone();
        }
        // Element-wise binary, fused and widening operations.
        const bool wideDst = f6 >= 0x30;
        const bool wideSrc2 = f6 == 0x34 || f6 == 0x36;               // vfwadd.w, vfwsub.w
        const unsigned dregs = wideDst ? (t.lmul + 1 > 0 ? 1u << (t.lmul + 1) : 1u) : lregs;
        if (wideDst && t.lmul + 1 > 3) return VILL;
        if (!aligned(rd, dregs) || !aligned(rs2, wideSrc2 ? dregs : lregs) || (!vf && !aligned(rs1, lregs))) return VILL;
        if (!vm && rd == 0) return VILL;
        if (wideDst) {                                                 // a narrower source may not overlap the destination
            auto clash = [&](unsigned s) { return s < rd + dregs && rd < s + lregs && !(s == rd + dregs - lregs && t.lmul >= 0); };
            if ((!wideSrc2 && clash(rs2)) || (!vf && clash(rs1))) return VILL;
        }
        const unsigned ob = wideDst ? 64 : sew;
        for (uint64_t i = h.vstart; i < vl; i++) {
            if (!active(i)) continue;
            uint64_t a = vget(h, rs2, i, wideSrc2 ? 8 : sB), b = op1(i), d = vget(h, rd, i, ob / 8), r;
            if (wideDst) {
                if (!wideSrc2) a = fpu::sToD((uint32_t)a, fl);
                b = fpu::sToD((uint32_t)b, fl);
            }
            switch (f6) {
                case 0x00: case 0x30: case 0x34: r = ar(fpu::Op::Add, a, b, 0, ob); break;
                case 0x02: case 0x32: case 0x36: r = ar(fpu::Op::Sub, a, b, 0, ob); break;
                case 0x27: if (!vf) return VILL; r = ar(fpu::Op::Sub, b, a, 0, ob); break;  // vfrsub
                case 0x04: r = fmin(a, b, false, ob); break;
                case 0x06: r = fmin(a, b, true, ob); break;
                case 0x08: r = sew == 32 ? fpu::sgnj<uint32_t>((uint32_t)a, (uint32_t)b, 0) : fpu::sgnj<uint64_t>(a, b, 0); break;
                case 0x09: r = sew == 32 ? fpu::sgnj<uint32_t>((uint32_t)a, (uint32_t)b, 1) : fpu::sgnj<uint64_t>(a, b, 1); break;
                case 0x0a: r = sew == 32 ? fpu::sgnj<uint32_t>((uint32_t)a, (uint32_t)b, 2) : fpu::sgnj<uint64_t>(a, b, 2); break;
                case 0x20: r = ar(fpu::Op::Div, a, b, 0, ob); break;
                case 0x21: if (!vf) return VILL; r = ar(fpu::Op::Div, b, a, 0, ob); break;  // vfrdiv
                case 0x24: case 0x38: r = ar(fpu::Op::Mul, a, b, 0, ob); break;
                // vd = +-(vs1 * vd) +- vs2
                case 0x28: r = ar(fpu::Op::Madd, b, d, a, ob); break;    // vfmadd
                case 0x29: r = ar(fpu::Op::Nmadd, b, d, a, ob); break;   // vfnmadd
                case 0x2a: r = ar(fpu::Op::Msub, b, d, a, ob); break;    // vfmsub
                case 0x2b: r = ar(fpu::Op::Nmsub, b, d, a, ob); break;   // vfnmsub
                // vd = +-(vs1 * vs2) +- vd
                case 0x2c: case 0x3c: r = ar(fpu::Op::Madd, b, a, d, ob); break;   // vfmacc
                case 0x2d: case 0x3d: r = ar(fpu::Op::Nmadd, b, a, d, ob); break;  // vfnmacc
                case 0x2e: case 0x3e: r = ar(fpu::Op::Msub, b, a, d, ob); break;   // vfmsac
                case 0x2f: case 0x3f: r = ar(fpu::Op::Nmsub, b, a, d, ob); break;  // vfnmsac
                default: return VILL;
            }
            vset(h, rd, i, ob / 8, r);
        }
        return fdone();
    }

    // -------------------------------------------- integer
    const bool opi = f3 == 0 || f3 == 3 || f3 == 4;                    // OPIVV, OPIVI, OPIVX
    const bool vv = f3 == 0 || f3 == 2;
    // Shifts, scaling shifts and clips take the immediate unsigned.
    const bool uimmOp = f6 == 0x25 || (f6 >= 0x28 && f6 <= 0x2f);
    auto op1 = [&](uint64_t i) -> uint64_t {
        return vv ? vget(h, rs1, i, sB) : f3 == 3 ? (uimmOp ? uimm : simm) : xs;
    };
    auto done = [&] { h.vstart = 0; dirty(); return VOK; };
    auto sx = [&](uint64_t v) { return (int64_t)vsext(v, sew); };
    const uint64_t smax = smask >> 1, smin = ~smax & smask;

    if (opi) {
        // vmv<nr>r.v
        if (f6 == 0x27 && f3 == 3) {
            const unsigned nr = rs1 + 1;
            if ((nr & (nr - 1)) || nr > 8 || !vm || (rd % nr) || (rs2 % nr)) return VILL;
            const uint64_t evl = (uint64_t)nr * vlenb / sB;
            for (uint64_t i = h.vstart; i < evl; i++) vset(h, rd, i, sB, vget(h, rs2, i, sB));
            return done();
        }
        // Widening reductions vwredsumu, vwredsum.
        if (f6 == 0x30 || f6 == 0x31) {
            if (f3 != 0 || h.vstart || sew == 64) return VILL;
            if (vl == 0) return done();
            uint64_t acc = vget(h, rs1, 0, 2 * sB);
            for (uint64_t i = 0; i < vl; i++) {
                if (!active(i)) continue;
                uint64_t e = vget(h, rs2, i, sB);
                acc += f6 == 0x31 ? (uint64_t)sx(e) : e;
            }
            vset(h, rd, 0, 2 * sB, acc & vmaskw(2 * sew));
            return done();
        }
        // Gathers and slides.
        if (f6 == 0x0c || (f6 == 0x0e && f3 == 0)) {                  // vrgather, vrgatherei16
            const bool ei16 = f6 == 0x0e;
            unsigned iregs = lregs;
            if (ei16) {
                int ie = t.lmul + 4 - (int)__builtin_ctz(sew);
                if (ie > 3 || ie < -3) return VILL;
                iregs = ie > 0 ? 1u << ie : 1u;
            }
            if (!aligned(rd, lregs) || !aligned(rs2, lregs) || (vv && !aligned(rs1, iregs))) return VILL;
            if ((rd < rs2 + lregs && rs2 < rd + lregs) || (vv && rd < rs1 + iregs && rs1 < rd + lregs)) return VILL;
            if (!vm && rd == 0) return VILL;
            const uint64_t vlmax = vlmaxOf(h.vlen, t);
            for (uint64_t i = h.vstart; i < vl; i++) {
                if (!active(i)) continue;
                uint64_t idx = ei16 ? vget(h, rs1, i, 2) : vv ? vget(h, rs1, i, sB) : f3 == 3 ? uimm : h.ax(h.x[rs1]);
                vset(h, rd, i, sB, idx < vlmax ? vget(h, rs2, idx, sB) : 0);
            }
            return done();
        }
        if ((f6 == 0x0e || f6 == 0x0f) && f3 != 0) {                  // vslideup, vslidedown
            if (!aligned(rd, lregs) || !aligned(rs2, lregs)) return VILL;
            if (f6 == 0x0e && rd < rs2 + lregs && rs2 < rd + lregs) return VILL;
            if (!vm && rd == 0) return VILL;
            const uint64_t off = f3 == 3 ? uimm : h.ax(h.x[rs1]);
            const uint64_t vlmax = vlmaxOf(h.vlen, t);
            if (f6 == 0x0e) {
                for (uint64_t i = std::max<uint64_t>(h.vstart, off); i < vl; i++)
                    if (active(i)) vset(h, rd, i, sB, vget(h, rs2, i - off, sB));
            } else {
                for (uint64_t i = h.vstart; i < vl; i++) {
                    if (!active(i)) continue;
                    uint64_t s = i + off;
                    vset(h, rd, i, sB, (s >= i && s < vlmax) ? vget(h, rs2, s, sB) : 0);
                }
            }
            return done();
        }
        // Add with carry and subtract with borrow; their carry-outs.
        if (f6 >= 0x10 && f6 <= 0x13) {
            const bool sub = f6 >= 0x12, maskOut = f6 & 1;
            if (sub && f3 == 3) return VILL;
            if (!maskOut && (vm || rd == 0 || !aligned(rd, lregs))) return VILL;  // vadc/vsbc need v0
            if (!aligned(rs2, lregs) || (vv && !aligned(rs1, lregs))) return VILL;
            for (uint64_t i = h.vstart; i < vl; i++) {
                const uint64_t a = vget(h, rs2, i, sB), b = op1(i);
                const uint64_t c = vm ? 0 : vbit(h, 0, i);
                if (!maskOut) {
                    vset(h, rd, i, sB, (sub ? a - b - c : a + b + c) & smask);
                } else {
                    bool co;
                    if (!sub) {
                        const uint64_t s = (a + b) & smask, s2 = (s + c) & smask;
                        co = s < a || s2 < s;
                        if (sew == 64) co = (a + b < a) || (a + b + c < a + b);
                    } else {
                        co = a < b || (a - b) < c;
                        if (sew == 64) co = a < b || (a == b && c) || ((a - b) < c);
                    }
                    vsetbit(h, rd, i, co);
                }
            }
            return done();
        }
        if (f6 == 0x17) {                                              // vmerge, vmv.v.*
            if ((vm && rs2) || !aligned(rd, lregs) || (vv && !aligned(rs1, lregs)) || (!vm && !aligned(rs2, lregs))) return VILL;
            if (!vm && rd == 0) return VILL;
            for (uint64_t i = h.vstart; i < vl; i++)
                vset(h, rd, i, sB, vm || vbit(h, 0, i) ? op1(i) : vget(h, rs2, i, sB));
            return done();
        }
        if (f6 >= 0x18 && f6 <= 0x1f) {                               // integer compares
            if ((f6 == 0x1e || f6 == 0x1f) && vv) return VILL;
            if ((f6 == 0x1a || f6 == 0x1b) && f3 == 3) return VILL;    // no vmsltu.vi, vmslt.vi
            if (!aligned(rs2, lregs) || (vv && !aligned(rs1, lregs))) return VILL;
            for (uint64_t i = h.vstart; i < vl; i++) {
                if (!active(i)) continue;
                const uint64_t a = vget(h, rs2, i, sB), b = op1(i);
                bool r;
                switch (f6) {
                    case 0x18: r = a == b; break;
                    case 0x19: r = a != b; break;
                    case 0x1a: r = a < b; break;
                    case 0x1b: r = sx(a) < sx(b); break;
                    case 0x1c: r = a <= b; break;
                    case 0x1d: r = sx(a) <= sx(b); break;
                    case 0x1e: r = a > b; break;
                    default: r = sx(a) > sx(b); break;
                }
                vsetbit(h, rd, i, r);
            }
            return done();
        }
        // Narrowing shifts and clips: vs2 is 2*SEW.
        if (f6 >= 0x2c && f6 <= 0x2f) {
            if (sew == 64 || t.lmul + 1 > 3) return VILL;
            const unsigned wregs = t.lmul + 1 > 0 ? 1u << (t.lmul + 1) : 1u;
            if (!aligned(rd, lregs) || !aligned(rs2, wregs) || (vv && !aligned(rs1, lregs))) return VILL;
            if (rd < rs2 + wregs && rs2 < rd + lregs && rd != rs2) return VILL;
            if (!vm && rd == 0) return VILL;
            const unsigned w = 2 * sew;
            for (uint64_t i = h.vstart; i < vl; i++) {
                if (!active(i)) continue;
                const uint64_t a = vget(h, rs2, i, 2 * sB);
                const unsigned s = (unsigned)(op1(i) & (w - 1));
                uint64_t r;
                if (f6 == 0x2c) r = a >> s;
                else if (f6 == 0x2d) r = (uint64_t)((int64_t)vsext(a, w) >> s);
                else if (f6 == 0x2e) {                                 // vnclipu
                    uint64_t q = (a >> s) + vround(a, s, h.vxrm);
                    if (q > smask) { q = smask; h.vxsat = 1; }
                    r = q;
                } else {                                               // vnclip
                    int64_t sa = (int64_t)vsext(a, w);
                    int64_t q = (sa >> s) + (int64_t)vround((uint64_t)sa, s, h.vxrm);
                    if (q > (int64_t)smax) { q = (int64_t)smax; h.vxsat = 1; }
                    if (q < (int64_t)vsext(smin, sew)) { q = (int64_t)vsext(smin, sew); h.vxsat = 1; }
                    r = (uint64_t)q;
                }
                vset(h, rd, i, sB, r & smask);
            }
            return done();
        }
        // Single-width element-wise.
        if (!aligned(rd, lregs) || !aligned(rs2, lregs) || (vv && !aligned(rs1, lregs))) return VILL;
        if (!vm && rd == 0) return VILL;
        for (uint64_t i = h.vstart; i < vl; i++) {
            if (!active(i)) continue;
            const uint64_t a = vget(h, rs2, i, sB), b = op1(i);
            const unsigned sh = (unsigned)(b & (sew - 1));
            uint64_t r;
            switch (f6) {
                case 0x00: r = a + b; break;                                    // vadd
                case 0x02: if (f3 == 3) return VILL; r = a - b; break;          // vsub
                case 0x03: if (vv) return VILL; r = b - a; break;               // vrsub
                case 0x04: if (f3 == 3) return VILL; r = a < b ? a : b; break;  // vminu
                case 0x05: if (f3 == 3) return VILL; r = sx(a) < sx(b) ? a : b; break;
                case 0x06: if (f3 == 3) return VILL; r = a > b ? a : b; break;
                case 0x07: if (f3 == 3) return VILL; r = sx(a) > sx(b) ? a : b; break;
                case 0x09: r = a & b; break;
                case 0x0a: r = a | b; break;
                case 0x0b: r = a ^ b; break;
                case 0x20: { uint64_t s = (a + b) & smask; if (s < a) { s = smask; h.vxsat = 1; } r = s; break; }  // vsaddu
                case 0x21: {                                                    // vsadd
                    int64_t s = sx(a) + sx(b);
                    if (sew == 64) {
                        int64_t sa = (int64_t)a, sb = (int64_t)b;
                        if (__builtin_add_overflow(sa, sb, &s)) { s = sa < 0 ? INT64_MIN : INT64_MAX; h.vxsat = 1; }
                    } else if (s > (int64_t)smax) { s = (int64_t)smax; h.vxsat = 1; }
                    else if (s < (int64_t)vsext(smin, sew)) { s = (int64_t)vsext(smin, sew); h.vxsat = 1; }
                    r = (uint64_t)s;
                    break;
                }
                case 0x22: if (f3 == 3) return VILL; r = a >= b ? a - b : (h.vxsat = 1, 0); break;  // vssubu
                case 0x23: {                                                    // vssub
                    if (f3 == 3) return VILL;
                    int64_t s;
                    if (sew == 64) {
                        int64_t sa = (int64_t)a, sb = (int64_t)b;
                        if (__builtin_sub_overflow(sa, sb, &s)) { s = sa < 0 ? INT64_MIN : INT64_MAX; h.vxsat = 1; }
                    } else {
                        s = sx(a) - sx(b);
                        if (s > (int64_t)smax) { s = (int64_t)smax; h.vxsat = 1; }
                        else if (s < (int64_t)vsext(smin, sew)) { s = (int64_t)vsext(smin, sew); h.vxsat = 1; }
                    }
                    r = (uint64_t)s;
                    break;
                }
                case 0x25: r = a << sh; break;                                  // vsll
                case 0x27: {                                                    // vsmul
                    if (f3 == 3) return VILL;
                    const __int128 p = (__int128)sx(a) * (__int128)sx(b);
                    const unsigned d = sew - 1;
                    const unsigned __int128 up = (unsigned __int128)p;
                    // round (p >> (sew-1)) by vxrm on the bits shifted out
                    uint64_t lo = (uint64_t)up;
                    uint64_t rb;
                    {
                        const uint64_t bit = (uint64_t)(up >> (d - 1)) & 1;
                        const uint64_t rest = d >= 2 ? lo & ((1ull << (d - 1)) - 1) : 0;
                        const uint64_t lsb = (uint64_t)(up >> d) & 1;
                        switch (h.vxrm & 3) {
                            case 0: rb = bit; break;
                            case 1: rb = bit & ((rest != 0) | lsb); break;
                            case 2: rb = 0; break;
                            default: rb = !lsb && (bit | (rest != 0)); break;
                        }
                    }
                    __int128 q = (p >> d) + (__int128)rb;
                    const __int128 hi = (__int128)(int64_t)vsext(smax, 64), lw = -hi - 1;
                    if (q > hi) { q = hi; h.vxsat = 1; }
                    if (q < lw) { q = lw; h.vxsat = 1; }
                    r = (uint64_t)q;
                    break;
                }
                case 0x28: r = a >> sh; break;                                  // vsrl
                case 0x29: r = (uint64_t)(sx(a) >> sh); break;                  // vsra
                case 0x2a: r = (a >> sh) + vround(a, sh, h.vxrm); break;        // vssrl
                case 0x2b: r = (uint64_t)(sx(a) >> sh) + vround((uint64_t)sx(a), sh, h.vxrm); break;  // vssra
                default: return VILL;
            }
            vset(h, rd, i, sB, r & smask);
        }
        return done();
    }

    // f3 == 2 (OPMVV) or 6 (OPMVX)
    if (f3 != 2 && f3 != 6) return VILL;
    // Reductions.
    if (f6 <= 0x07 && vv) {
        if (h.vstart) return VILL;
        if (!aligned(rs2, lregs)) return VILL;
        if (vl == 0) return done();
        uint64_t acc = vget(h, rs1, 0, sB);
        for (uint64_t i = 0; i < vl; i++) {
            if (!active(i)) continue;
            const uint64_t e = vget(h, rs2, i, sB);
            switch (f6) {
                case 0: acc += e; break;
                case 1: acc &= e; break;
                case 2: acc |= e; break;
                case 3: acc ^= e; break;
                case 4: acc = e < acc ? e : acc; break;
                case 5: acc = sx(e) < sx(acc) ? e : acc; break;
                case 6: acc = e > acc ? e : acc; break;
                default: acc = sx(e) > sx(acc) ? e : acc; break;
            }
            acc &= smask;
        }
        vset(h, rd, 0, sB, acc);
        return done();
    }
    if (f6 == 0x10) {
        if (vv) {                                                      // VWXUNARY0
            if (rs1 == 0) {                                            // vmv.x.s
                if (!vm) return VILL;
                xw(rd, vsext(vget(h, rs2, 0, sB), sew));
                h.vstart = 0;
                return VOK;
            }
            if (rs1 == 16 || rs1 == 17) {                              // vcpop.m, vfirst.m
                if (h.vstart) return VILL;
                int64_t first = -1;
                uint64_t n = 0;
                for (uint64_t i = 0; i < vl; i++)
                    if (active(i) && vbit(h, rs2, i)) { n++; if (first < 0) first = (int64_t)i; }
                xw(rd, rs1 == 16 ? n : (uint64_t)first);
                h.vstart = 0;
                return VOK;
            }
            return VILL;
        }
        if (rs2 || !vm) return VILL;                                   // vmv.s.x
        if (h.vstart < vl) vset(h, rd, 0, sB, xs);
        return done();
    }
    if (f6 == 0x12 && vv) {                                            // vzext, vsext
        const unsigned k = rs1;
        if (k < 2 || k > 7) return VILL;
        const unsigned frac = k <= 3 ? 8 : k <= 5 ? 4 : 2;
        const bool sgn = k & 1;
        if (sew / frac < 8) return VILL;
        const int semul = t.lmul - (int)__builtin_ctz(frac);
        if (semul < -3) return VILL;
        const unsigned sregs = semul > 0 ? 1u << semul : 1u;
        if (!aligned(rd, lregs) || !aligned(rs2, sregs)) return VILL;
        if (rs2 < rd + lregs && rd < rs2 + sregs && !(rs2 == rd + lregs - sregs && semul >= 0)) return VILL;
        if (!vm && rd == 0) return VILL;
        const unsigned nb = sB / frac;
        for (uint64_t i = h.vstart; i < vl; i++) {
            if (!active(i)) continue;
            uint64_t e = vget(h, rs2, i, nb);
            vset(h, rd, i, sB, (sgn ? vsext(e, nb * 8) : e) & smask);
        }
        return done();
    }
    if (f6 == 0x14 && vv) {                                            // VMUNARY0
        const unsigned k = rs1;
        if (k == 1 || k == 2 || k == 3) {                              // vmsbf, vmsof, vmsif
            if (h.vstart || rd == rs2 || (!vm && rd == 0)) return VILL;
            bool seen = false;
            for (uint64_t i = 0; i < vl; i++) {
                if (!active(i)) continue;
                const bool b = vbit(h, rs2, i);
                bool r;
                if (k == 1) r = !seen && !b;
                else if (k == 2) r = !seen && b;
                else r = !seen;
                if (b) seen = true;
                vsetbit(h, rd, i, r);
            }
            return done();
        }
        if (k == 16) {                                                 // viota
            if (h.vstart || !aligned(rd, lregs) || (rd < rs2 + 1 && rs2 < rd + lregs) || (!vm && rd == 0)) return VILL;
            uint64_t n = 0;
            for (uint64_t i = 0; i < vl; i++) {
                if (!active(i)) continue;
                vset(h, rd, i, sB, n & smask);
                if (vbit(h, rs2, i)) n++;
            }
            return done();
        }
        if (k == 17) {                                                 // vid
            if (rs2 || !aligned(rd, lregs) || (!vm && rd == 0)) return VILL;
            for (uint64_t i = h.vstart; i < vl; i++)
                if (active(i)) vset(h, rd, i, sB, i & smask);
            return done();
        }
        return VILL;
    }
    if (f6 == 0x17) {                                                  // vcompress
        if (!vv || !vm || h.vstart || !aligned(rd, lregs) || !aligned(rs2, lregs)) return VILL;
        if ((rd < rs2 + lregs && rs2 < rd + lregs) || (rd <= rs1 && rs1 < rd + lregs)) return VILL;
        uint64_t j = 0;
        for (uint64_t i = 0; i < vl; i++)
            if (vbit(h, rs1, i)) vset(h, rd, j++, sB, vget(h, rs2, i, sB));
        return done();
    }
    if (f6 >= 0x18 && f6 <= 0x1f) {                                   // mask logical
        if (!vv || !vm) return VILL;
        for (uint64_t i = h.vstart; i < vl; i++) {
            const bool a = vbit(h, rs2, i), b = vbit(h, rs1, i);
            bool r;
            switch (f6) {
                case 0x18: r = a && !b; break;     // vmandn
                case 0x19: r = a && b; break;      // vmand
                case 0x1a: r = a || b; break;      // vmor
                case 0x1b: r = a != b; break;      // vmxor
                case 0x1c: r = a || !b; break;     // vmorn
                case 0x1d: r = !(a && b); break;   // vmnand
                case 0x1e: r = !(a || b); break;   // vmnor
                default: r = a == b; break;        // vmxnor
            }
            vsetbit(h, rd, i, r);
        }
        return done();
    }
    if (f6 == 0x0e || f6 == 0x0f) {                                   // vslide1up, vslide1down
        if (vv || !aligned(rd, lregs) || !aligned(rs2, lregs)) return VILL;
        if (f6 == 0x0e && rd < rs2 + lregs && rs2 < rd + lregs) return VILL;
        if (!vm && rd == 0) return VILL;
        for (uint64_t i = h.vstart; i < vl; i++) {
            if (!active(i)) continue;
            uint64_t v;
            if (f6 == 0x0e) v = i == 0 ? xs : vget(h, rs2, i - 1, sB);
            else v = i + 1 < vl ? vget(h, rs2, i + 1, sB) : xs;
            vset(h, rd, i, sB, v);
        }
        return done();
    }
    // Widening: the destination is 2*SEW.
    if (f6 >= 0x30) {
        if (sew == 64 || t.lmul + 1 > 3) return VILL;
        const unsigned wregs = t.lmul + 1 > 0 ? 1u << (t.lmul + 1) : 1u;
        const bool wide2 = f6 >= 0x34 && f6 <= 0x37;                   // .wv, .wx: vs2 is wide
        if (f6 == 0x39 || (f6 == 0x3e && vv)) return VILL;
        if (!aligned(rd, wregs) || !aligned(rs2, wide2 ? wregs : lregs) || (vv && !aligned(rs1, lregs))) return VILL;
        auto clash = [&](unsigned s) { return s < rd + wregs && rd < s + lregs && !(s == rd + wregs - lregs && t.lmul >= 0); };
        if ((!wide2 && clash(rs2)) || (vv && clash(rs1))) return VILL;
        if (!vm && rd == 0) return VILL;
        const unsigned w = 2 * sew;
        const uint64_t wm = vmaskw(w);
        for (uint64_t i = h.vstart; i < vl; i++) {
            if (!active(i)) continue;
            uint64_t a = vget(h, rs2, i, wide2 ? 2 * sB : sB), b = op1(i), d = vget(h, rd, i, 2 * sB), r;
            const uint64_t ua = a, ub = b & smask;
            const uint64_t sa = wide2 ? a : vsext(a, sew), sb = vsext(b, sew);
            switch (f6) {
                case 0x30: r = ua + ub; break;                         // vwaddu
                case 0x31: r = sa + sb; break;                         // vwadd
                case 0x32: r = ua - ub; break;                         // vwsubu
                case 0x33: r = sa - sb; break;                         // vwsub
                case 0x34: r = ua + ub; break;                         // vwaddu.w
                case 0x35: r = a + sb; break;                          // vwadd.w
                case 0x36: r = ua - ub; break;                         // vwsubu.w
                case 0x37: r = a - sb; break;                          // vwsub.w
                case 0x38: r = ua * ub; break;                         // vwmulu
                case 0x3a: r = sa * ub; break;                         // vwmulsu
                case 0x3b: r = sa * sb; break;                         // vwmul
                case 0x3c: r = d + ua * ub; break;                     // vwmaccu
                case 0x3d: r = d + sa * sb; break;                     // vwmacc
                case 0x3e: r = d + ua * sb; break;                     // vwmaccus: unsigned rs1 x signed vs2
                default: r = d + sb * ua; break;                       // vwmaccsu: signed vs1 x unsigned vs2
            }
            if (f6 == 0x3e) r = d + (uint64_t)((int64_t)vsext(a, sew) * (int64_t)ub);
            if (f6 == 0x3f) r = d + (uint64_t)((int64_t)sb * (int64_t)(a & smask));
            vset(h, rd, i, 2 * sB, r & wm);
        }
        return done();
    }
    // Single-width: averaging, multiply, divide, multiply-add.
    if (!aligned(rd, lregs) || !aligned(rs2, lregs) || (vv && !aligned(rs1, lregs))) return VILL;
    if (!vm && rd == 0) return VILL;
    for (uint64_t i = h.vstart; i < vl; i++) {
        if (!active(i)) continue;
        const uint64_t a = vget(h, rs2, i, sB), b = op1(i) & smask, d = vget(h, rd, i, sB);
        uint64_t r;
        auto mulhi = [&](bool sa, bool sb) -> uint64_t {
            __int128 x = sa ? (__int128)sx(a) : (__int128)a, y = sb ? (__int128)sx(b) : (__int128)b;
            return (uint64_t)((x * y) >> sew);
        };
        auto mulhu = [&]() -> uint64_t { return (uint64_t)(((unsigned __int128)a * b) >> sew); };
        switch (f6) {
            case 0x08: {                                               // vaaddu
                unsigned __int128 s = (unsigned __int128)a + b;
                r = (uint64_t)(s >> 1) + vround((uint64_t)s, 1, h.vxrm);
                break;
            }
            case 0x09: {                                               // vaadd
                __int128 s = (__int128)sx(a) + sx(b);
                r = (uint64_t)(s >> 1) + vround((uint64_t)s, 1, h.vxrm);
                break;
            }
            case 0x0a: {                                               // vasubu
                __int128 s = (__int128)a - (__int128)b;
                r = (uint64_t)(s >> 1) + vround((uint64_t)s, 1, h.vxrm);
                break;
            }
            case 0x0b: {                                               // vasub
                __int128 s = (__int128)sx(a) - sx(b);
                r = (uint64_t)(s >> 1) + vround((uint64_t)s, 1, h.vxrm);
                break;
            }
            case 0x20: r = b == 0 ? smask : a / b; break;                                       // vdivu
            case 0x21: r = b == 0 ? smask : (a == smin && sx(b) == -1) ? smin : (uint64_t)(sx(a) / sx(b)); break;  // vdiv
            case 0x22: r = b == 0 ? a : a % b; break;                                           // vremu
            case 0x23: r = b == 0 ? a : (a == smin && sx(b) == -1) ? 0 : (uint64_t)(sx(a) % sx(b)); break;  // vrem
            case 0x24: r = mulhu(); break;                                                      // vmulhu
            case 0x25: r = a * b; break;                                                        // vmul
            case 0x26: r = mulhi(true, false); break;                                           // vmulhsu
            case 0x27: r = mulhi(true, true); break;                                            // vmulh
            case 0x29: r = b * d + a; break;                                                    // vmadd
            case 0x2b: r = a - b * d; break;                                                    // vnmsub
            case 0x2d: r = b * a + d; break;                                                    // vmacc
            case 0x2f: r = d - b * a; break;                                                    // vnmsac
            default: return VILL;
        }
        vset(h, rd, i, sB, r & smask);
    }
    return done();
}
