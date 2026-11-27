// a64dec.inc - decoded instructions for the AArch64 processors: each 4 KiB
// page of RAM that code runs from keeps its instructions decoded, the
// commonest forms with a function of their own and their fields taken out,
// the rest handed to exec(). Part of a64machine.hpp.
//
// A decoded page stays right while its bytes do. Pages decoded from are
// watched (wtrack.hpp): the first store to one, from a processor or a
// device, counts a write on it, and before the next instruction the pages
// written are dropped, to be decoded again from what they hold now. So code
// runs as it would fetched afresh each time, changed or not. Where memory
// cannot be watched, nothing is decoded ahead and every instruction goes
// through exec().
#pragma once

namespace vp {
namespace a64 {

namespace dec {

using M = Machine;
inline uint64_t sx(uint64_t v, unsigned n) {
    return n == 1 ? (uint64_t)(int64_t)(int8_t)v : n == 2 ? (uint64_t)(int64_t)(int16_t)v : (uint64_t)(int64_t)(int32_t)v;
}

inline void exec(M& m, Cpu& c, const Op& op, uint64_t pc) { m.exec(c, op.insn, pc); }
inline void nop(M&, Cpu&, const Op&, uint64_t) {}

// ---- data processing, immediate
template <bool sf, bool sub, bool s>
void addImm(M&, Cpu& c, const Op& op, uint64_t) {
    const uint64_t a = M::XS(c, op.rn);
    const uint64_t r = sub ? M::addc(c, a, ~op.imm, 1, sf, s) : M::addc(c, a, op.imm, 0, sf, s);
    if (s) M::setX(c, op.rd, r, sf); else M::setXS(c, op.rd, r, sf);
}
template <bool sf, unsigned opc>
void logImm(M&, Cpu& c, const Op& op, uint64_t) {
    const uint64_t a = M::X(c, op.rn);
    if (opc == 3) { const uint64_t r = a & op.imm; M::nzLogic(c, r, sf); M::setX(c, op.rd, r, sf); return; }
    M::setXS(c, op.rd, opc == 0 ? a & op.imm : opc == 1 ? a | op.imm : a ^ op.imm, sf);
}
inline void movConst(M&, Cpu& c, const Op& op, uint64_t) { c.x[op.rd] = op.imm; }
template <bool sf>
void movk(M&, Cpu& c, const Op& op, uint64_t) { M::setX(c, op.rd, (c.x[op.rd] & ~op.imm2) | op.imm, sf); }
inline void adr(M&, Cpu& c, const Op& op, uint64_t pc) { c.x[op.rd] = pc + op.imm; }
inline void adrp(M&, Cpu& c, const Op& op, uint64_t pc) { c.x[op.rd] = (pc & ~0xfffull) + op.imm; }
template <bool sf, unsigned opc>
void bitfield(M&, Cpu& c, const Op& op, uint64_t) {
    constexpr unsigned w = sf ? 64 : 32;
    const uint64_t wm = op.imm, tm = op.imm2;
    const uint64_t src = M::X(c, op.rn), dst = opc == 1 ? M::X(c, op.rd) : 0;
    const uint64_t bot = (dst & ~wm) | (M::ror(src & M::ones(w), op.rm, w) & wm);
    const uint64_t top = opc == 0 ? ((src >> op.ra & 1) ? M::ones(w) : 0) : dst;
    M::setX(c, op.rd, (top & ~tm) | (bot & tm), sf);
}
template <bool sf>
void extr(M&, Cpu& c, const Op& op, uint64_t) {
    constexpr unsigned w = sf ? 64 : 32;
    const unsigned lsb = op.ra;
    const uint64_t hi = M::X(c, op.rn) & M::ones(w), lo = M::X(c, op.rm) & M::ones(w);
    M::setX(c, op.rd, lsb ? ((lo >> lsb) | (hi << (w - lsb))) & M::ones(w) : lo, sf);
}

// ---- data processing, register
template <bool sf>
void movReg(M&, Cpu& c, const Op& op, uint64_t) { M::setX(c, op.rd, M::X(c, op.rm), sf); }
template <bool sf, unsigned opc, bool inv>
void logReg(M&, Cpu& c, const Op& op, uint64_t) {
    uint64_t b = M::shiftReg(M::X(c, op.rm), op.ra, (unsigned)op.imm, sf);
    if (inv) b = ~b;
    const uint64_t a = M::X(c, op.rn);
    uint64_t r;
    switch (opc) {
        case 0: r = a & b; break;
        case 1: r = a | b; break;
        case 2: r = a ^ b; break;
        default: r = a & b; M::nzLogic(c, r, sf); break;
    }
    M::setX(c, op.rd, r, sf);
}
template <bool sf, bool sub, bool s>
void addReg(M&, Cpu& c, const Op& op, uint64_t) {
    const uint64_t b = M::shiftReg(M::X(c, op.rm), op.ra, (unsigned)op.imm, sf), a = M::X(c, op.rn);
    M::setX(c, op.rd, sub ? M::addc(c, a, ~b, 1, sf, s) : M::addc(c, a, b, 0, sf, s), sf);
}
template <bool sf, bool sub, bool s>
void addExt(M&, Cpu& c, const Op& op, uint64_t) {
    const uint64_t b = M::extendReg(M::X(c, op.rm), op.ra, (unsigned)op.imm), a = M::XS(c, op.rn);
    const uint64_t r = sub ? M::addc(c, a, ~b, 1, sf, s) : M::addc(c, a, b, 0, sf, s);
    if (s) M::setX(c, op.rd, r, sf); else M::setXS(c, op.rd, r, sf);
}
template <bool sf, unsigned o>
void csel(M&, Cpu& c, const Op& op, uint64_t) {
    uint64_t r;
    if (M::cond(c, op.ra)) r = M::X(c, op.rn);
    else {
        r = M::X(c, op.rm);
        if (o == 1) r = r + 1;
        else if (o == 2) r = ~r;
        else if (o == 3) r = 0 - r;
    }
    M::setX(c, op.rd, r, sf);
}
template <bool sf, bool sub, bool immForm>
void ccmp(M&, Cpu& c, const Op& op, uint64_t) {
    const uint64_t a = M::X(c, op.rn), b = immForm ? op.rm : M::X(c, op.rm);
    if (M::cond(c, op.ra)) {
        if (sub) M::addc(c, a, ~b, 1, sf, true);
        else M::addc(c, a, b, 0, sf, true);
    } else c.nzcv = (uint32_t)op.imm;
}
template <bool sf, bool msub>
void madd(M&, Cpu& c, const Op& op, uint64_t) {
    const uint64_t a = M::X(c, op.rn), b = M::X(c, op.rm), acc = M::X(c, op.ra);
    M::setX(c, op.rd, msub ? acc - a * b : acc + a * b, sf);
}

// ---- branches
inline void b(M&, Cpu& c, const Op& op, uint64_t pc) { c.pc = pc + op.imm; }
inline void bl(M&, Cpu& c, const Op& op, uint64_t pc) { c.x[30] = pc + 4; c.pc = pc + op.imm; }
inline void bcond(M&, Cpu& c, const Op& op, uint64_t pc) { if (M::cond(c, op.ra)) c.pc = pc + op.imm; }
template <bool sf, bool nz>
void cbz(M&, Cpu& c, const Op& op, uint64_t pc) {
    uint64_t v = M::X(c, op.rd);
    if (!sf) v = (uint32_t)v;
    if ((v == 0) != nz) c.pc = pc + op.imm;
}
template <bool nz>
void tbz(M&, Cpu& c, const Op& op, uint64_t pc) {
    if ((M::X(c, op.rd) >> op.ra & 1) == (unsigned)nz) c.pc = pc + op.imm;
}
inline void br(M&, Cpu& c, const Op& op, uint64_t) { c.pc = M::X(c, op.rn); }
inline void blr(M&, Cpu& c, const Op& op, uint64_t pc) { const uint64_t t = M::X(c, op.rn); c.x[30] = pc + 4; c.pc = t; }

// ---- loads and stores of the general registers
// kind: 0 store, 1 load, 2 load sign-extended to 64 bits, 3 to 32 bits
inline bool spOk(M& m, Cpu& c, unsigned rn, uint64_t base, uint64_t pc) {
    if (rn == 31 && (base & 15) && (c.sctlr >> (c.el ? 3 : 4) & 1)) {
        m.exception(c, EC_SP_ALIGN, 0, pc);
        return false;
    }
    return true;
}
template <unsigned n, int kind>
inline bool ldst1(M& m, Cpu& c, unsigned rt, uint64_t a, uint64_t& v) {
    if (kind == 0) return m.wr(c, a, n, M::X(c, rt));
    if (!m.rd(c, a, n, v)) return false;
    if (kind == 2) v = sx(v, n);
    else if (kind == 3) v = (uint32_t)sx(v, n);
    return true;
}
// mode 0: base + imm, 1: post-index, 2: pre-index
template <unsigned n, int kind, int mode>
void ldst(M& m, Cpu& c, const Op& op, uint64_t pc) {
    const unsigned rt = op.rd, rn = op.rn;
    const uint64_t base = M::XS(c, rn), imm = op.imm;
    if (!spOk(m, c, rn, base, pc)) return;
    uint64_t v;
    if (!ldst1<n, kind>(m, c, rt, mode == 1 ? base : base + imm, v)) return;
    if (mode) M::setXS(c, rn, base + imm, true);
    if (kind) M::setX(c, rt, v, true);
}
template <unsigned n, int kind>
void ldstReg(M& m, Cpu& c, const Op& op, uint64_t pc) {
    const unsigned rt = op.rd, rn = op.rn;
    const uint64_t base = M::XS(c, rn);
    const uint64_t a = base + M::extendReg(M::X(c, op.rm), op.ra, (unsigned)op.imm);
    if (!spOk(m, c, rn, base, pc)) return;
    uint64_t v;
    if (!ldst1<n, kind>(m, c, rt, a, v)) return;
    if (kind) M::setX(c, rt, v, true);
}
// pairs; mode as above
template <unsigned n, bool load, int mode>
void pair(M& m, Cpu& c, const Op& op, uint64_t pc) {
    const unsigned rt = op.rd, rt2 = op.ra, rn = op.rn;
    const uint64_t base = M::XS(c, rn), off = op.imm;
    if (!spOk(m, c, rn, base, pc)) return;
    const uint64_t a = mode == 1 ? base : base + off;
    if (load) {
        uint64_t v1, v2;
        if (!m.rd(c, a, n, v1) || !m.rd(c, a + n, n, v2)) return;
        M::setX(c, rt, v1, true);
        M::setX(c, rt2, v2, true);
    } else {
        // across two pages: exec() translates both before writing either
        if ((a & 0xfff) + 2 * n > 4096) { m.exec(c, op.insn, pc); return; }
        const uint64_t v1 = M::X(c, rt), v2 = M::X(c, rt2);
        if (!m.wr(c, a, n, v1) || !m.wr(c, a + n, n, v2)) return;
    }
    if (mode) M::setXS(c, rn, base + off, true);
}

template <unsigned n, int kind>
Op::Fn ldstFn(int mode) {
    return mode == 0 ? &ldst<n, kind, 0> : mode == 1 ? &ldst<n, kind, 1> : &ldst<n, kind, 2>;
}
template <int kind>
Op::Fn ldstFn(unsigned size, int mode) {
    switch (size) {
        case 0: return ldstFn<1, kind>(mode);
        case 1: return ldstFn<2, kind>(mode);
        case 2: return ldstFn<4, kind>(mode);
        default: return ldstFn<8, kind>(mode);
    }
}
template <int kind>
Op::Fn ldstRegFn(unsigned size) {
    switch (size) {
        case 0: return &ldstReg<1, kind>;
        case 1: return &ldstReg<2, kind>;
        case 2: return &ldstReg<4, kind>;
        default: return &ldstReg<8, kind>;
    }
}
template <unsigned n, bool load>
Op::Fn pairFn(int mode) {
    return mode == 0 ? &pair<n, load, 0> : mode == 1 ? &pair<n, load, 1> : &pair<n, load, 2>;
}

// The handler for an instruction and its fields; exec for the rest.
inline void decode(uint32_t i, Op& op) {
    op = Op{&exec, i, (uint8_t)(i & 31), (uint8_t)((i >> 5) & 31), (uint8_t)((i >> 16) & 31), 0, 0, 0};
    const bool sf = i >> 31;
    const unsigned rd = i & 31;
#define SF(f) (sf ? &f<true> : &f<false>)
#define SF2(f, a) (sf ? &f<true, a> : &f<false, a>)
#define SF3(f, a, b) (sf ? &f<true, a, b> : &f<false, a, b>)
    if ((i & 0x1f800000) == 0x11000000) {  // ADD/SUB (immediate)
        const bool sub = i >> 30 & 1, s = i >> 29 & 1;
        op.imm = (uint64_t)((i >> 10) & 0xfff) << ((i >> 22 & 1) ? 12 : 0);
        op.fn = sub ? (s ? SF3(addImm, true, true) : SF3(addImm, true, false))
                    : (s ? SF3(addImm, false, true) : SF3(addImm, false, false));
        return;
    }
    if ((i & 0x1f800000) == 0x12000000) {  // logical (immediate)
        const unsigned n = i >> 22 & 1;
        if (!sf && n) return;
        uint64_t wm, tm;
        if (!M::bitMasks(n, (i >> 10) & 63, (i >> 16) & 63, true, sf ? 64 : 32, wm, tm)) return;
        op.imm = wm;
        switch ((i >> 29) & 3) {
            case 0: op.fn = SF2(logImm, 0); break;
            case 1: op.fn = SF2(logImm, 1); break;
            case 2: op.fn = SF2(logImm, 2); break;
            default: op.fn = SF2(logImm, 3); break;
        }
        return;
    }
    if ((i & 0x1f800000) == 0x12800000) {  // move wide
        const unsigned opc = (i >> 29) & 3, hw = (i >> 21) & 3;
        if (opc == 1 || (!sf && hw > 1)) return;
        const uint64_t imm = (uint64_t)((i >> 5) & 0xffff) << (16 * hw);
        if (opc == 3) { op.imm = imm; op.imm2 = 0xffffull << (16 * hw); op.fn = rd == 31 ? &nop : SF(movk); return; }
        uint64_t r = opc == 0 ? ~imm : imm;
        if (!sf) r = (uint32_t)r;
        op.imm = r;
        op.fn = rd == 31 ? &nop : &movConst;
        return;
    }
    if ((i & 0x1f000000) == 0x10000000) {  // ADR, ADRP
        const int64_t imm = (int64_t)(((uint64_t)((i >> 5) & 0x7ffff) << 2 | ((i >> 29) & 3)) << 43) >> 43;
        if (rd == 31) { op.fn = &nop; return; }
        if (i >> 31) { op.imm = (uint64_t)(imm << 12); op.fn = &adrp; }
        else { op.imm = (uint64_t)imm; op.fn = &adr; }
        return;
    }
    if ((i & 0x1f800000) == 0x13000000) {  // bitfield
        const unsigned opc = (i >> 29) & 3, n = i >> 22 & 1, immr = (i >> 16) & 63, imms = (i >> 10) & 63;
        if (opc == 3 || n != (unsigned)sf || (!sf && (immr > 31 || imms > 31))) return;
        uint64_t wm, tm;
        if (!M::bitMasks(n, imms, immr, false, sf ? 64 : 32, wm, tm)) return;
        op.imm = wm; op.imm2 = tm; op.rm = (uint8_t)immr; op.ra = (uint8_t)imms;
        op.fn = opc == 0 ? SF2(bitfield, 0) : opc == 1 ? SF2(bitfield, 1) : SF2(bitfield, 2);
        return;
    }
    if ((i & 0x1f800000) == 0x13800000) {  // EXTR
        const unsigned n = i >> 22 & 1, lsb = (i >> 10) & 63;
        if (n != (unsigned)sf || (i >> 29 & 3) || (i >> 21 & 1) || (!sf && lsb > 31)) return;
        op.ra = (uint8_t)lsb;
        op.fn = SF(extr);
        return;
    }
    if ((i & 0x1f000000) == 0x0a000000) {  // logical (shifted register)
        const unsigned sh = (i >> 22) & 3, amt = (i >> 10) & 63, opc = (i >> 29) & 3;
        const bool inv = i >> 21 & 1;
        if (!sf && amt > 31) return;
        op.ra = (uint8_t)sh; op.imm = amt;
        if (opc == 1 && !inv && !amt && op.rn == 31) { op.fn = SF(movReg); return; }
        switch (opc << 1 | inv) {
            case 0: op.fn = SF3(logReg, 0, false); break;
            case 1: op.fn = SF3(logReg, 0, true); break;
            case 2: op.fn = SF3(logReg, 1, false); break;
            case 3: op.fn = SF3(logReg, 1, true); break;
            case 4: op.fn = SF3(logReg, 2, false); break;
            case 5: op.fn = SF3(logReg, 2, true); break;
            case 6: op.fn = SF3(logReg, 3, false); break;
            default: op.fn = SF3(logReg, 3, true); break;
        }
        return;
    }
    if ((i & 0x1f200000) == 0x0b000000) {  // add/sub (shifted register)
        const unsigned sh = (i >> 22) & 3, amt = (i >> 10) & 63;
        if (sh == 3 || (!sf && amt > 31)) return;
        const bool sub = i >> 30 & 1, s = i >> 29 & 1;
        op.ra = (uint8_t)sh; op.imm = amt;
        op.fn = sub ? (s ? SF3(addReg, true, true) : SF3(addReg, true, false))
                    : (s ? SF3(addReg, false, true) : SF3(addReg, false, false));
        return;
    }
    if ((i & 0x1f200000) == 0x0b200000) {  // add/sub (extended register)
        const unsigned opt = (i >> 13) & 7, amt = (i >> 10) & 7;
        if (((i >> 22) & 3) || amt > 4) return;
        const bool sub = i >> 30 & 1, s = i >> 29 & 1;
        op.ra = (uint8_t)opt; op.imm = amt;
        op.fn = sub ? (s ? SF3(addExt, true, true) : SF3(addExt, true, false))
                    : (s ? SF3(addExt, false, true) : SF3(addExt, false, false));
        return;
    }
    if ((i & 0x1fe00000) == 0x1a800000) {  // CSEL, CSINC, CSINV, CSNEG
        if (i >> 29 & 1 || (i >> 11 & 1)) return;
        op.ra = (uint8_t)((i >> 12) & 15);
        switch ((i >> 30 & 1) << 1 | (i >> 10 & 1)) {
            case 0: op.fn = SF2(csel, 0); break;
            case 1: op.fn = SF2(csel, 1); break;
            case 2: op.fn = SF2(csel, 2); break;
            default: op.fn = SF2(csel, 3); break;
        }
        return;
    }
    if ((i & 0x1fe00000) == 0x1a400000) {  // CCMN, CCMP
        if (!(i >> 29 & 1) || (i >> 10 & 1) || (i >> 4 & 1)) return;
        const bool sub = i >> 30 & 1, immForm = i >> 11 & 1;
        op.ra = (uint8_t)((i >> 12) & 15); op.imm = (uint64_t)(i & 15) << 28;
        op.fn = sub ? (immForm ? SF3(ccmp, true, true) : SF3(ccmp, true, false))
                    : (immForm ? SF3(ccmp, false, true) : SF3(ccmp, false, false));
        return;
    }
    if ((i & 0x7fe00000) == 0x1b000000) {  // MADD, MSUB
        op.ra = (uint8_t)((i >> 10) & 31);
        op.fn = (i >> 15 & 1) ? SF2(madd, true) : SF2(madd, false);
        return;
    }
    // branches
    if ((i & 0x7c000000) == 0x14000000) {  // B, BL
        op.imm = (uint64_t)((int64_t)((uint64_t)(i & 0x3ffffff) << 38) >> 36);
        op.fn = (i >> 31) ? &bl : &b;
        return;
    }
    if ((i & 0xfe000000) == 0x54000000) {  // B.cond
        op.imm = (uint64_t)((int64_t)((uint64_t)((i >> 5) & 0x7ffff) << 45) >> 43);
        op.ra = (uint8_t)(i & 15);
        op.fn = &bcond;
        return;
    }
    if ((i & 0x7e000000) == 0x34000000) {  // CBZ, CBNZ
        op.imm = (uint64_t)((int64_t)((uint64_t)((i >> 5) & 0x7ffff) << 45) >> 43);
        op.fn = (i >> 24 & 1) ? SF2(cbz, true) : SF2(cbz, false);
        return;
    }
    if ((i & 0x7e000000) == 0x36000000) {  // TBZ, TBNZ
        op.ra = (uint8_t)((i >> 31) << 5 | ((i >> 19) & 31));
        op.imm = (uint64_t)((int64_t)((uint64_t)((i >> 5) & 0x3fff) << 50) >> 48);
        op.fn = (i >> 24 & 1) ? &tbz<true> : &tbz<false>;
        return;
    }
    if ((i & 0xffbffc1f) == 0xd61f0000) {  // BR, RET (opc 0, 2)
        op.fn = &br;
        return;
    }
    if ((i & 0xfffffc1f) == 0xd63f0000) {  // BLR
        op.fn = &blr;
        return;
    }
    if ((i & 0xfffff01f) == 0xd503201f) {  // hints: those that do nothing
        const unsigned h = (i >> 5) & 0x7f;
        if (h < 2 || h > 5) op.fn = &nop;
        return;
    }
    // loads and stores of the general registers
    if ((i & 0x3f000000) == 0x39000000 || (i & 0x3f200000) == 0x38000000 || (i & 0x3f200c00) == 0x38200800) {
        const unsigned size = i >> 30, opc = (i >> 22) & 3;
        int kind;
        if (opc == 0) kind = 0;
        else if (opc == 1) kind = 1;
        else if (size == 3 || (size == 2 && opc == 3)) return;  // PRFM, unallocated
        else kind = opc == 3 ? 3 : 2;
        Op::Fn f;
        if ((i & 0x3f200c00) == 0x38200800) {  // register offset
            const unsigned opt = (i >> 13) & 7;
            if (!(opt & 2)) return;
            op.ra = (uint8_t)opt; op.imm = (i >> 12 & 1) ? size : 0;
            f = kind == 0 ? ldstRegFn<0>(size) : kind == 1 ? ldstRegFn<1>(size) : kind == 2 ? ldstRegFn<2>(size) : ldstRegFn<3>(size);
        } else {
            int mode = 0;
            if (i >> 24 & 1) op.imm = (uint64_t)((i >> 10) & 0xfff) << size;
            else {
                const unsigned m = (i >> 10) & 3;
                if (m == 2) return;  // unprivileged
                mode = m == 0 ? 0 : m == 1 ? 1 : 2;
                op.imm = (uint64_t)((int64_t)((uint64_t)((i >> 12) & 0x1ff) << 55) >> 55);
            }
            f = kind == 0 ? ldstFn<0>(size, mode) : kind == 1 ? ldstFn<1>(size, mode) : kind == 2 ? ldstFn<2>(size, mode) : ldstFn<3>(size, mode);
        }
        op.fn = f;
        return;
    }
    if ((i & 0x3e000000) == 0x28000000) {  // load/store pair
        const unsigned opc = i >> 30, mode = (i >> 23) & 3;
        const bool l = i >> 22 & 1;
        if (opc != 0 && opc != 2) return;  // LDPSW, STGP
        const unsigned n = opc == 2 ? 8 : 4;
        op.ra = (uint8_t)((i >> 10) & 31);
        op.imm = (uint64_t)(((int64_t)((uint64_t)((i >> 15) & 0x7f) << 57) >> 57) * (int64_t)n);
        const int md = mode == 1 ? 1 : mode == 3 ? 2 : 0;
        op.fn = n == 8 ? (l ? pairFn<8, true>(md) : pairFn<8, false>(md)) : (l ? pairFn<4, true>(md) : pairFn<4, false>(md));
        return;
    }
#undef SF
#undef SF2
#undef SF3
}

// The first run of an instruction: decoded, then run.
inline void first(M& m, Cpu& c, const Op& op, uint64_t pc) {
    Op& o = const_cast<Op&>(op);
    decode(op.insn, o);
    o.fn(m, c, o, pc);
}

}  // namespace dec

inline CodePage* Machine::codePage(Cpu& c, uint64_t pa, uint8_t* host) {
    pa &= ~0xfffull;
    auto it = c.code.find(pa);
    if (it != c.code.end()) return it->second.get();
    if (!decodeAhead_) return nullptr;
    if (c.code.size() >= CODE_PAGES) {  // enough: start again
        for (auto& e : c.code) c.deadCode.push_back(std::move(e.second));
        c.code.clear();
        for (auto& e : c.fastX) e.key = ~0ull;
    }
    Device* d = bus.find(pa);
    if (!d || !d->memory()) return nullptr;
    int wr = -2;
    uint32_t seen = 0;
    if (d->writable()) {
        std::lock_guard<std::mutex> l(watchMu_);
        auto r = watchIds_.find(d);
        if (r == watchIds_.end())
            r = watchIds_.emplace(d, (d->base & 4095) || (d->size & 4095) ? -1 : WriteWatch::add(d->memory(), d->size)).first;
        wr = r->second;
        if (wr < 0) { decodeAhead_ = false; return nullptr; }
        const size_t wp = (size_t)(host - WriteWatch::base(wr)) >> 12;
        if (!WriteWatch::watch(wr, wp)) return nullptr;
        seen = WriteWatch::gen(wr, wp);
    }
    // watched first, then read: a write after this counts
    auto p = std::make_unique<CodePage>();
    p->host = host; p->pa = pa; p->wr = wr; p->seen = seen;
    for (unsigned k = 0; k < 1024; k++) {
        uint32_t insn;
        memcpy(&insn, host + 4 * k, 4);
        p->op[k] = Op{&dec::first, insn, 0, 0, 0, 0, 0, 0};
    }
    CodePage* r = p.get();
    c.code.emplace(pa, std::move(p));
    return r;
}

// Something was written to a watched page: the pages written since they
// were decoded are dropped.
inline void Machine::codeSync(Cpu& c) {
    c.codeWrites = WriteWatch::writes();
    bool dropped = false;
    for (auto it = c.code.begin(); it != c.code.end();) {
        CodePage& p = *it->second;
        if (p.wr >= 0) {
            const size_t wp = (size_t)(p.host - WriteWatch::base(p.wr)) >> 12;
            if (!WriteWatch::readOnly(p.wr, wp) || WriteWatch::gen(p.wr, wp) != p.seen) {
                // the instruction running may be on it: freed between instructions
                c.deadCode.push_back(std::move(it->second));
                it = c.code.erase(it);
                dropped = true;
                continue;
            }
        }
        ++it;
    }
    if (dropped)
        for (auto& e : c.fastX) e.key = ~0ull;
}

}  // namespace a64
}  // namespace vp
