// a64jit.inc - blocks of AArch64 instructions translated to x86-64 code.
// Part of a64machine.hpp.
//
// A block starts where the run loop found an instruction that has started
// blocks often (jitHot_ times, 32), on a decoded page (a64dec.inc), and runs on
// to the first instruction that may go anywhere but on (an unconditional
// branch, a system instruction, an exception) or to the page's end, at most
// JIT_MAX instructions. A conditional branch does not end it: taken, the
// block leaves there.
//
// Each instruction is the decoded page's own function, called from the
// block (so a block does exactly what the decoded instructions would);
// what the block saves is the run loop's work between them. After each one
// the block leaves if the processor did not go on to the next instruction
// (a branch taken, an exception), if the run was asked to stop, or if
// anything wrote to a watched code page (codeSync before the next). It
// returns how many instructions ran; the run loop counts them as it counts
// its own. Interrupts, timers and the quantum's end are looked at between
// blocks, so a block is entered only if all of it fits in the quantum.
//
// The blocks of a page hang on its CodePage (CodePage::jit) and go with it
// when it is dropped; a full code buffer empties every processor's tables
// at once (jitGen_).
#pragma once

#include <cstddef>

namespace vp {
namespace a64 {

#if MVPLAT_JIT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
// The decoded forms translated to host code; the rest are called.
struct JitForm {
    enum Kind : uint8_t {
        NONE, NOP, MOVC, MOVK, ADR, ADRP, ADDI, LOGI, MOVR, LOGR, ADDR, CSEL, MADD,
        B, BL, BCOND, CBZ, TBZ, BR, BLR, LDST, PAIR, LDSTR, BFM, EXTR, ADDX, CCMP
    } kind = NONE;
    bool sf = false;
    uint8_t a = 0, b = 0, c = 0;  // the template's other arguments
};
inline const std::unordered_map<Op::Fn, JitForm>& jitForms() {
    static const std::unordered_map<Op::Fn, JitForm> t = [] {
        using K = JitForm::Kind;
        std::unordered_map<Op::Fn, JitForm> m;
        auto add = [&](Op::Fn f, K k, bool sf = false, int a = 0, int b = 0, int c = 0) {
            m[f] = JitForm{k, sf, (uint8_t)a, (uint8_t)b, (uint8_t)c};
        };
        add(&dec::nop, K::NOP); add(&dec::movConst, K::MOVC); add(&dec::adr, K::ADR); add(&dec::adrp, K::ADRP);
        add(&dec::b, K::B); add(&dec::bl, K::BL); add(&dec::bcond, K::BCOND); add(&dec::br, K::BR);
        add(&dec::blr, K::BLR);
        add(&dec::tbz<false>, K::TBZ, false, 0); add(&dec::tbz<true>, K::TBZ, false, 1);
#define BOTH(tmpl, k, ...) add(&dec::tmpl<false, ##__VA_ARGS__>, k, false, ##__VA_ARGS__); \
                           add(&dec::tmpl<true, ##__VA_ARGS__>, k, true, ##__VA_ARGS__)
        BOTH(movk, K::MOVK); BOTH(movReg, K::MOVR);
        BOTH(addImm, K::ADDI, false, false); BOTH(addImm, K::ADDI, false, true);
        BOTH(addImm, K::ADDI, true, false); BOTH(addImm, K::ADDI, true, true);
        BOTH(logImm, K::LOGI, 0); BOTH(logImm, K::LOGI, 1); BOTH(logImm, K::LOGI, 2); BOTH(logImm, K::LOGI, 3);
        BOTH(logReg, K::LOGR, 0, false); BOTH(logReg, K::LOGR, 0, true); BOTH(logReg, K::LOGR, 1, false);
        BOTH(logReg, K::LOGR, 1, true); BOTH(logReg, K::LOGR, 2, false); BOTH(logReg, K::LOGR, 2, true);
        BOTH(logReg, K::LOGR, 3, false); BOTH(logReg, K::LOGR, 3, true);
        BOTH(addReg, K::ADDR, false, false); BOTH(addReg, K::ADDR, false, true);
        BOTH(addReg, K::ADDR, true, false); BOTH(addReg, K::ADDR, true, true);
        BOTH(csel, K::CSEL, 0); BOTH(csel, K::CSEL, 1); BOTH(csel, K::CSEL, 2); BOTH(csel, K::CSEL, 3);
        BOTH(madd, K::MADD, false); BOTH(madd, K::MADD, true);
        BOTH(cbz, K::CBZ, false); BOTH(cbz, K::CBZ, true);
        BOTH(bitfield, K::BFM, 0); BOTH(bitfield, K::BFM, 2);  // SBFM, UBFM (BFM is called)
        BOTH(extr, K::EXTR);
        BOTH(addExt, K::ADDX, false, false); BOTH(addExt, K::ADDX, false, true);
        BOTH(addExt, K::ADDX, true, false); BOTH(addExt, K::ADDX, true, true);
        // a = sub, b = immediate form
        BOTH(ccmp, K::CCMP, false, false); BOTH(ccmp, K::CCMP, false, true);
        BOTH(ccmp, K::CCMP, true, false); BOTH(ccmp, K::CCMP, true, true);
#undef BOTH
        // a = bytes, b = kind, c = mode
#define LD(n, kd) add(&dec::ldst<n, kd, 0>, K::LDST, false, n, kd, 0); add(&dec::ldst<n, kd, 1>, K::LDST, false, n, kd, 1); \
                  add(&dec::ldst<n, kd, 2>, K::LDST, false, n, kd, 2)
        LD(1, 0); LD(2, 0); LD(4, 0); LD(8, 0); LD(1, 1); LD(2, 1); LD(4, 1); LD(8, 1);
        LD(1, 2); LD(2, 2); LD(4, 2); LD(1, 3); LD(2, 3);
#undef LD
#define LR(n, kd) add(&dec::ldstReg<n, kd>, K::LDSTR, false, n, kd)
        LR(1, 0); LR(2, 0); LR(4, 0); LR(8, 0); LR(1, 1); LR(2, 1); LR(4, 1); LR(8, 1);
        LR(1, 2); LR(2, 2); LR(4, 2); LR(1, 3); LR(2, 3);
#undef LR
        // a = bytes, b = load, c = mode
#define PR(n, l) add(&dec::pair<n, l, 0>, K::PAIR, false, n, l, 0); add(&dec::pair<n, l, 1>, K::PAIR, false, n, l, 1); \
                 add(&dec::pair<n, l, 2>, K::PAIR, false, n, l, 2)
        PR(4, false); PR(4, true); PR(8, false); PR(8, true);
#undef PR
        return m;
    }();
    return t;
}

// Called forms that only compute (never fault, branch or touch memory): no
// pc to set before them, nothing to look at after.
inline bool jitPure(const Op& op) {
    if (op.fn == &dec::exec || op.fn == &dec::first) return false;
    switch (profClass(op.insn)) {
        case PC_ADR: case PC_ADDSUB_IMM: case PC_LOGIC_IMM: case PC_MOVE_WIDE: case PC_BITFIELD: case PC_EXTRACT:
        case PC_ADDSUB_REG: case PC_LOGIC_REG: case PC_ADC: case PC_CCMP: case PC_CSEL: case PC_DP1: case PC_DP2:
        case PC_MUL: case PC_DIV: case PC_CRC:
            return true;
        default:
            return false;
    }
}

inline bool Machine::jitTranslate(Cpu& c, CodePage* cp, unsigned k, uint64_t pc, JitSlot& s) {
    Jit& j = *jit_[c.id];
    if (j.failed) return false;
    // the instructions: decoded, up to one that ends the block
    unsigned n = 0;
    while (k + n < 1024 && n < JIT_MAX) {
        Op& op = cp->op[k + n];
        if (op.fn == &dec::first) dec::decode(op.insn, op);
        n++;
        const ProfClass pc_ = profClass(op.insn);
        if (pc_ == PC_B || pc_ == PC_BR || pc_ == PC_SYSTEM || pc_ == PC_EXC || pc_ == PC_OTHER) break;
    }
    using R = Jit;
    using K = JitForm::Kind;
    const auto& forms = jitForms();
    // The block is for the exception level and stack pointer it was made
    // at (it reads SP and looks up the fast tables as these say).
    const unsigned el = c.el;
    const bool spsel = c.spsel, el0 = el == 0;
    const int32_t OX = (int32_t)offsetof(Cpu, x), OPC = (int32_t)offsetof(Cpu, pc),
                  OCUR = (int32_t)offsetof(Cpu, curPc), OCW = (int32_t)offsetof(Cpu, codeWrites),
                  ONZCV = (int32_t)offsetof(Cpu, nzcv), OFR = (int32_t)offsetof(Cpu, fastR),
                  OFW = (int32_t)offsetof(Cpu, fastW), OEL = (int32_t)offsetof(Cpu, el),
                  OSPSEL = (int32_t)offsetof(Cpu, spsel),
                  OSP = (int32_t)offsetof(Cpu, sp) + ((el && spsel) ? 8 : 0);
    const int32_t OEXIT = (int32_t)offsetof(Machine, exit_), OMON = (int32_t)offsetof(Machine, monitors_);

    // ---- emitting helpers (rbx = the processor, rbp = the machine,
    // r12 = the write counter's address; rax rcx rdx rsi rdi r8 r9 scratch)
    auto ldX = [&](int reg, unsigned r) {
        if (r == 31) { j.aluRR(R::A_XOR, reg, reg, false); return; }
        j.rex(true, reg, R::RBX); j.b(0x8b); j.mem(reg, R::RBX, OX + 8 * (int32_t)r);
    };
    auto ldXS = [&](int reg, unsigned r) {
        if (r != 31) { ldX(reg, r); return; }
        j.rex(true, reg, R::RBX); j.b(0x8b); j.mem(reg, R::RBX, OSP);
    };
    auto stRaw = [&](int32_t off, int reg) { j.rex(true, reg, R::RBX); j.b(0x89); j.mem(reg, R::RBX, off); };
    auto stX = [&](unsigned r, int reg, bool sf) {
        if (r == 31) return;
        if (!sf) j.movRR(reg, reg, false);
        stRaw(OX + 8 * (int32_t)r, reg);
    };
    auto stXS = [&](unsigned r, int reg, bool sf) {
        if (!sf) j.movRR(reg, reg, false);
        stRaw(r == 31 ? OSP : OX + 8 * (int32_t)r, reg);
    };
    auto stPc = [&](uint64_t v) { j.movI(R::RAX, v); stRaw(OPC, R::RAX); };
    // NZCV from the host's flags after an add (C = CF), a subtract (C = !CF)
    // or a logical operation (CF = OF = 0)
    auto flags = [&](bool sub) {
        j.setcc((R::Cc)8, R::RCX);              // N: sets
        j.setcc(R::CC_E, R::RDX);               // Z
        j.setcc(sub ? R::CC_AE : R::CC_B, R::R8);
        j.setcc((R::Cc)0, R::R9);               // V: seto
        j.movzxb(R::RCX, R::RCX); j.shiftI(4, R::RCX, 31, false);
        j.movzxb(R::RDX, R::RDX); j.shiftI(4, R::RDX, 30, false); j.aluRR(R::A_OR, R::RCX, R::RDX, false);
        j.movzxb(R::RDX, R::R8); j.shiftI(4, R::RDX, 29, false); j.aluRR(R::A_OR, R::RCX, R::RDX, false);
        j.movzxb(R::RDX, R::R9); j.shiftI(4, R::RDX, 28, false); j.aluRR(R::A_OR, R::RCX, R::RDX, false);
        j.rex(false, R::RCX, R::RBX); j.b(0x89); j.mem(R::RCX, R::RBX, ONZCV);
    };
    // A condition (C1.2.4): sets the host flags and gives the host
    // condition that holds when it does; -1 for always. Uses rdx, r8.
    auto cond = [&](unsigned cd) -> int {
        if ((cd >> 1) == 7) return -1;
        j.rex(false, R::RDX, R::RBX); j.b(0x8b); j.mem(R::RDX, R::RBX, ONZCV);
        int t;
        switch (cd >> 1) {
            case 0: case 1: case 2: case 3: {
                static const uint32_t bit[4] = {1u << 30, 1u << 29, 1u << 31, 1u << 28};  // Z C N V
                j.b(0xf7); j.modrr(0, R::RDX); j.d32(bit[cd >> 1]);   // test edx, bit
                t = R::CC_NE;
                break;
            }
            case 4:  // HI: C and not Z
                j.aluI(R::A_AND, R::RDX, (int32_t)(3u << 29), false);
                j.aluI(R::A_CMP, R::RDX, (int32_t)(1u << 29), false);
                t = R::CC_E;
                break;
            default:  // GE: N == V; GT: that and not Z
                j.movRR(R::R8, R::RDX, false);
                j.shiftI(5, R::R8, 3, false);
                j.aluRR(R::A_XOR, R::R8, R::RDX, false);
                if ((cd >> 1) == 6) {
                    j.aluI(R::A_AND, R::R8, (int32_t)(1u << 28), false);
                    j.aluI(R::A_AND, R::RDX, (int32_t)(1u << 30), false);
                    j.aluRR(R::A_OR, R::R8, R::RDX, false);
                } else {
                    j.aluI(R::A_AND, R::R8, (int32_t)(1u << 28), false);
                }
                t = R::CC_E;
                break;
        }
        return (cd & 1) ? (t ^ 1) : t;
    };
    // the shift of a register operand on rcx
    auto shift = [&](unsigned type, unsigned amt, bool sf) {
        if (!sf) j.movRR(R::RCX, R::RCX, false);
        if (!amt) return;
        static const int ext[4] = {4, 5, 7, 1};  // shl shr sar ror
        j.shiftI(ext[type & 3], R::RCX, amt, sf);
    };
    // an extended register operand (option, then shift) on rcx
    auto extend = [&](unsigned opt, unsigned sh) {
        switch (opt) {
            case 0: j.movzxb(R::RCX, R::RCX); break;
            case 1: j.rex(false, R::RCX, R::RCX); j.b(0x0f); j.b(0xb7); j.modrr(R::RCX, R::RCX); break;
            case 2: j.movRR(R::RCX, R::RCX, false); break;
            case 4: j.rex(true, R::RCX, R::RCX); j.b(0x0f); j.b(0xbe); j.modrr(R::RCX, R::RCX); break;
            case 5: j.rex(true, R::RCX, R::RCX); j.b(0x0f); j.b(0xbf); j.modrr(R::RCX, R::RCX); break;
            case 6: j.movsxd(R::RCX, R::RCX); break;
            default: break;
        }
        if (sh) j.shiftI(4, R::RCX, sh);
    };
    auto callOp = [&](const Op& op, uint64_t at) {
        j.movRR(R::RDI, R::RBP);
        j.movRR(R::RSI, R::RBX);
        j.movI(R::RDX, (uint64_t)(uintptr_t)&op);
        j.movI(R::RCX, at);
        j.movI(R::RAX, (uint64_t)(uintptr_t)op.fn);
        j.b(0xff); j.modrr(2, R::RAX);  // call rax
    };
    // leaving the block: how many ran, and the pc if the code has not set it
    struct Exit { uint8_t* rel; unsigned count; bool setPc; uint64_t pc; };
    std::vector<Exit> exits;
    // a translated instruction's way out to its decoded function
    struct Slow { std::vector<uint8_t*> rels; const Op* op; uint64_t at; unsigned i; uint8_t* back; };
    std::vector<Slow> slows;
    // after a call: leave if it did not go on, a stop was asked for, or a
    // watched page was written
    auto afterCall = [&](uint64_t at, unsigned count) {
        j.movI(R::RAX, at + 4);
        j.rex(true, R::RAX, R::RBX); j.b(0x3b); j.mem(R::RAX, R::RBX, OPC);
        exits.push_back({j.jcc(R::CC_NE), count, false, 0});
        j.b(0x80); j.mem(7, R::RBP, OEXIT); j.b(0);
        exits.push_back({j.jcc(R::CC_NE), count, false, 0});
        j.rex(true, R::RAX, R::R12); j.b(0x8b); j.mem(R::RAX, R::R12, 0);
        j.rex(true, R::RAX, R::RBX); j.b(0x3b); j.mem(R::RAX, R::RBX, OCW);
        exits.push_back({j.jcc(R::CC_NE), count, false, 0});
    };
    // The host address of an access of nb bytes at the address in rdi, in
    // r9, or a jump to the slow way (added to sl). Uses rcx rdx r8.
    auto fastAddr = [&](bool write, unsigned nb, std::vector<uint8_t*>& sl) {
        const int32_t base = write ? OFW : OFR;
        j.movRR(R::RCX, R::RDI); j.shiftI(5, R::RCX, 12);
        j.movRR(R::RDX, R::RCX, false); j.aluI(R::A_AND, R::RDX, (int32_t)(Cpu::FAST_N - 1), false);
        j.shiftI(4, R::RDX, 5);
        j.aluRR(R::A_ADD, R::RDX, R::RBX);
        j.movRR(R::R8, R::RCX); j.aluRR(R::A_ADD, R::R8, R::RCX);
        if (el0) j.aluI(R::A_OR, R::R8, 1);
        j.rex(true, R::R8, R::RDX); j.b(0x3b); j.mem(R::R8, R::RDX, base);   // cmp r8, key
        sl.push_back(j.jcc(R::CC_NE));
        j.movRR(R::RCX, R::RDI, false); j.aluI(R::A_AND, R::RCX, 0xfff, false);
        j.aluI(R::A_CMP, R::RCX, (int32_t)(4096 - nb), false);
        sl.push_back(j.jcc(R::CC_A));
        j.rex(true, R::R9, R::RDX); j.b(0x8b); j.mem(R::R9, R::RDX, base + 8);  // the host page
        j.aluRR(R::A_ADD, R::R9, R::RCX);
        if (write) {  // exclusives held: the decoded function clears them
            j.b(0x83); j.mem(7, R::RBP, OMON); j.b(0);
            sl.push_back(j.jcc(R::CC_NE));
        }
    };
    // load nb bytes at [r9 + d] into reg: kind 1 zero-extended, 2 sign-extended to 64, 3 to 32
    auto load = [&](int reg, unsigned nb, int kind, int32_t d) {
        switch (nb) {
            case 8: j.rex(true, reg, R::R9); j.b(0x8b); break;
            case 4: if (kind == 2) { j.rex(true, reg, R::R9); j.b(0x63); } else { j.rex(false, reg, R::R9); j.b(0x8b); } break;
            case 2: j.rex(kind == 2, reg, R::R9); j.b(0x0f); j.b(kind == 1 ? 0xb7 : 0xbf); break;
            default: j.rex(kind == 2, reg, R::R9); j.b(0x0f); j.b(kind == 1 ? 0xb6 : 0xbe); break;
        }
        j.mem(reg, R::R9, d);
    };
    auto store = [&](int reg, unsigned nb, int32_t d) {
        if (nb == 2) j.b(0x66);
        j.rex(nb == 8, reg, R::R9, 0, nb == 1 && reg >= 4);
        j.b(nb == 1 ? 0x88 : 0x89);
        j.mem(reg, R::R9, d);
    };
    // the base of a load or store in rsi, the address in rdi; to the slow
    // way if SP is the base and not 16-aligned (spOk decides)
    auto addrOf = [&](const Op& op, int mode, std::vector<uint8_t*>& sl) {
        ldXS(R::RSI, op.rn);
        if (op.rn == 31) {
            j.b(0xf7); j.modrr(0, R::RSI); j.d32(15);  // test esi, 15
            sl.push_back(j.jcc(R::CC_NE));
        }
        j.movRR(R::RDI, R::RSI);
        if (mode != 1 && op.imm) {
            if ((int64_t)op.imm == (int32_t)op.imm) j.aluI(R::A_ADD, R::RDI, (int32_t)op.imm);
            else { j.movI(R::RAX, op.imm); j.aluRR(R::A_ADD, R::RDI, R::RAX); }
        }
    };
    auto writeback = [&](const Op& op, int mode) {
        if (!mode) return;
        if ((int64_t)op.imm == (int32_t)op.imm) j.aluI(R::A_ADD, R::RSI, (int32_t)op.imm);
        else { j.movI(R::R8, op.imm); j.aluRR(R::A_ADD, R::RSI, R::R8); }
        stXS(op.rn, R::RSI, true);
    };
    auto watchCheck = [&](unsigned count, uint64_t next) {
        j.rex(true, R::RAX, R::R12); j.b(0x8b); j.mem(R::RAX, R::R12, 0);
        j.rex(true, R::RAX, R::RBX); j.b(0x3b); j.mem(R::RAX, R::RBX, OCW);
        exits.push_back({j.jcc(R::CC_NE), count, true, next});
    };

    j.p = j.buf + j.used;
    uint8_t* start = j.p;
    // uint32_t block(Machine* rdi, Cpu* rsi) (3 pushes: rsp 16-aligned)
    j.b(0x53); j.b(0x55); j.b(0x41); j.b(0x54);
    j.movRR(R::RBX, R::RSI);
    j.movRR(R::RBP, R::RDI);
    j.movI(R::R12, (uint64_t)(uintptr_t)WriteWatch::writesPtr());
    // made for another exception level or stack pointer: none run
    std::vector<uint8_t*> wrong;
    j.b(0x83); j.mem(7, R::RBX, OEL); j.b((uint8_t)el);
    wrong.push_back(j.jcc(R::CC_NE));
    j.b(0x80); j.mem(7, R::RBX, OSPSEL); j.b(spsel ? 1 : 0);
    wrong.push_back(j.jcc(R::CC_NE));

    bool pcSet = false;  // the last instruction set the pc
    for (unsigned i = 0; i < n; i++) {
        const Op& op = cp->op[k + i];
        const uint64_t at = pc + 4ull * i;
        const bool last = i + 1 == n;
        auto it = forms.find(op.fn);
        const JitForm f = it == forms.end() ? JitForm{} : it->second;
        const bool sf = f.sf;
        pcSet = false;
        std::vector<uint8_t*> sl;
        switch (f.kind) {
            case K::NONE:
                if (jitPure(op)) { callOp(op, at); break; }
                stPc(at + 4);
                j.movI(R::RAX, at); stRaw(OCUR, R::RAX);
                callOp(op, at);
                if (!last) afterCall(at, i + 1);
                pcSet = true;
                break;
            case K::NOP: break;
            case K::MOVC: j.movI(R::RAX, op.imm); stRaw(OX + 8 * op.rd, R::RAX); break;
            case K::ADR: j.movI(R::RAX, at + op.imm); stRaw(OX + 8 * op.rd, R::RAX); break;
            case K::ADRP: j.movI(R::RAX, (at & ~0xfffull) + op.imm); stRaw(OX + 8 * op.rd, R::RAX); break;
            case K::MOVK:
                if (op.rd == 31) break;
                j.rex(true, R::RAX, R::RBX); j.b(0x8b); j.mem(R::RAX, R::RBX, OX + 8 * op.rd);
                j.movI(R::RCX, ~op.imm2); j.aluRR(R::A_AND, R::RAX, R::RCX);
                j.movI(R::RCX, op.imm); j.aluRR(R::A_OR, R::RAX, R::RCX);
                stX(op.rd, R::RAX, sf);
                break;
            case K::MOVR: ldX(R::RAX, op.rm); stX(op.rd, R::RAX, sf); break;
            case K::ADDI: {
                const bool sub = f.a, setf = f.b;
                ldXS(R::RAX, op.rn);
                j.aluI(sub ? R::A_SUB : R::A_ADD, R::RAX, (int32_t)op.imm, sf);
                if (setf) { flags(sub); stX(op.rd, R::RAX, sf); }
                else stXS(op.rd, R::RAX, sf);
                break;
            }
            case K::LOGI: {
                static const R::Alu alu[4] = {R::A_AND, R::A_OR, R::A_XOR, R::A_AND};
                ldX(R::RAX, op.rn);
                j.movI(R::RCX, op.imm);
                j.aluRR(alu[f.a], R::RAX, R::RCX, sf);
                if (f.a == 3) { flags(false); stX(op.rd, R::RAX, sf); }
                else stXS(op.rd, R::RAX, sf);
                break;
            }
            case K::LOGR: {
                static const R::Alu alu[4] = {R::A_AND, R::A_OR, R::A_XOR, R::A_AND};
                ldX(R::RCX, op.rm);
                shift(op.ra, (unsigned)op.imm, sf);
                if (f.b) { j.rex(sf, 0, R::RCX); j.b(0xf7); j.modrr(2, R::RCX); }  // not
                ldX(R::RAX, op.rn);
                j.aluRR(alu[f.a], R::RAX, R::RCX, sf);
                if (f.a == 3) flags(false);
                stX(op.rd, R::RAX, sf);
                break;
            }
            case K::ADDR: {
                const bool sub = f.a, setf = f.b;
                ldX(R::RCX, op.rm);
                shift(op.ra, (unsigned)op.imm, sf);
                ldX(R::RAX, op.rn);
                j.aluRR(sub ? R::A_SUB : R::A_ADD, R::RAX, R::RCX, sf);
                if (setf) flags(sub);
                stX(op.rd, R::RAX, sf);
                break;
            }
            case K::CSEL: {
                ldX(R::RAX, op.rn);
                ldX(R::RCX, op.rm);
                if (f.a == 1) j.aluI(R::A_ADD, R::RCX, 1);
                else if (f.a == 2) { j.rex(true, 0, R::RCX); j.b(0xf7); j.modrr(2, R::RCX); }
                else if (f.a == 3) { j.rex(true, 0, R::RCX); j.b(0xf7); j.modrr(3, R::RCX); }
                const int cc = cond(op.ra);
                if (cc >= 0) { j.rex(true, R::RAX, R::RCX); j.b(0x0f); j.b((uint8_t)(0x40 + (cc ^ 1))); j.modrr(R::RAX, R::RCX); }
                stX(op.rd, R::RAX, sf);
                break;
            }
            case K::MADD:
                ldX(R::RAX, op.rn);
                ldX(R::RCX, op.rm);
                j.rex(sf, R::RAX, R::RCX); j.b(0x0f); j.b(0xaf); j.modrr(R::RAX, R::RCX);
                ldX(R::RDX, op.ra);
                if (f.a) { j.aluRR(R::A_SUB, R::RDX, R::RAX); j.movRR(R::RAX, R::RDX); }
                else j.aluRR(R::A_ADD, R::RAX, R::RDX);
                stX(op.rd, R::RAX, sf);
                break;
            case K::B: stPc(at + op.imm); pcSet = true; break;
            case K::BL: j.movI(R::RAX, at + 4); stRaw(OX + 8 * 30, R::RAX); stPc(at + op.imm); pcSet = true; break;
            case K::BR: ldX(R::RAX, op.rn); stRaw(OPC, R::RAX); pcSet = true; break;
            case K::BLR:
                ldX(R::RAX, op.rn);
                j.movI(R::RCX, at + 4); stRaw(OX + 8 * 30, R::RCX);
                stRaw(OPC, R::RAX);
                pcSet = true;
                break;
            case K::BCOND: {
                const int cc = cond(op.ra);
                if (cc < 0) { exits.push_back({j.jmp(), i + 1, true, at + op.imm}); }
                else exits.push_back({j.jcc((R::Cc)cc), i + 1, true, at + op.imm});
                break;
            }
            case K::CBZ:
                ldX(R::RAX, op.rd);
                j.rex(sf, R::RAX, R::RAX); j.b(0x85); j.modrr(R::RAX, R::RAX);
                exits.push_back({j.jcc(f.a ? R::CC_NE : R::CC_E), i + 1, true, at + op.imm});
                break;
            case K::TBZ:
                ldX(R::RAX, op.rd);
                j.rex(true, 0, R::RAX); j.b(0x0f); j.b(0xba); j.modrr(4, R::RAX); j.b((uint8_t)op.ra);  // bt
                exits.push_back({j.jcc(f.a ? R::CC_B : R::CC_AE), i + 1, true, at + op.imm});
                break;
            case K::BFM: {
                // UBFM: ror(src) & wmask & tmask; SBFM: that, and the sign
                // (bit imms of src) outside tmask
                const unsigned w = sf ? 64 : 32;
                const uint64_t ones = sf ? ~0ull : 0xffffffffull;
                ldX(R::RAX, op.rn);
                if (!sf) j.movRR(R::RAX, R::RAX, false);
                if (f.a == 0) j.movRR(R::RDX, R::RAX);
                if (op.rm) j.shiftI(1, R::RAX, op.rm, sf);  // ror
                j.movI(R::RCX, op.imm & op.imm2 & ones);
                j.aluRR(R::A_AND, R::RAX, R::RCX);
                if (f.a == 0) {
                    j.shiftI(4, R::RDX, 63 - op.ra);
                    j.shiftI(7, R::RDX, 63);
                    j.movI(R::RCX, ~op.imm2 & ones);
                    j.aluRR(R::A_AND, R::RDX, R::RCX);
                    j.aluRR(R::A_OR, R::RAX, R::RDX);
                }
                (void)w;
                stX(op.rd, R::RAX, sf);
                break;
            }
            case K::EXTR: {
                // (hi:lo) >> lsb: shrd lo, hi, lsb
                ldX(R::RAX, op.rm);
                ldX(R::RCX, op.rn);
                if (op.ra) { j.rex(sf, R::RCX, R::RAX); j.b(0x0f); j.b(0xac); j.modrr(R::RCX, R::RAX); j.b(op.ra); }
                stX(op.rd, R::RAX, sf);
                break;
            }
            case K::ADDX: {
                const bool sub = f.a, setf = f.b;
                ldX(R::RCX, op.rm);
                extend(op.ra, (unsigned)op.imm);
                ldXS(R::RAX, op.rn);
                j.aluRR(sub ? R::A_SUB : R::A_ADD, R::RAX, R::RCX, sf);
                if (setf) { flags(sub); stX(op.rd, R::RAX, sf); }
                else stXS(op.rd, R::RAX, sf);
                break;
            }
            case K::CCMP: {
                const bool sub = f.a, immF = f.b;
                const int cc = cond(op.ra);
                uint8_t* no = cc >= 0 ? j.jcc((R::Cc)(cc ^ 1)) : nullptr;
                ldX(R::RAX, op.rn);
                if (immF) j.aluI(sub ? R::A_SUB : R::A_ADD, R::RAX, (int32_t)op.rm, sf);
                else { ldX(R::RCX, op.rm); j.aluRR(sub ? R::A_SUB : R::A_ADD, R::RAX, R::RCX, sf); }
                flags(sub);
                if (no) {
                    uint8_t* done = j.jmp();
                    Jit::fix(no, j.p);
                    j.rex(false, 0, R::RBX); j.b(0xc7); j.mem(0, R::RBX, ONZCV); j.d32((uint32_t)op.imm);
                    Jit::fix(done, j.p);
                }
                break;
            }
            case K::LDSTR: {
                const unsigned nb = f.a;
                const int kind = f.b;
                ldXS(R::RSI, op.rn);
                if (op.rn == 31) {
                    j.b(0xf7); j.modrr(0, R::RSI); j.d32(15);
                    sl.push_back(j.jcc(R::CC_NE));
                }
                ldX(R::RCX, op.rm);
                extend(op.ra, (unsigned)op.imm);
                j.movRR(R::RDI, R::RSI);
                j.aluRR(R::A_ADD, R::RDI, R::RCX);
                fastAddr(kind == 0, nb, sl);
                if (kind == 0) {
                    ldX(R::RAX, op.rd);
                    store(R::RAX, nb, 0);
                    if (!last) watchCheck(i + 1, at + 4);
                } else {
                    load(R::RAX, nb, kind, 0);
                    stX(op.rd, R::RAX, true);
                }
                break;
            }
            case K::LDST: {
                const unsigned nb = f.a;
                const int kind = f.b, mode = f.c;
                addrOf(op, mode, sl);
                fastAddr(kind == 0, nb, sl);
                if (kind == 0) {
                    ldX(R::RAX, op.rd);
                    store(R::RAX, nb, 0);
                    writeback(op, mode);
                    if (!last) watchCheck(i + 1, at + 4);
                } else {
                    load(R::RAX, nb, kind, 0);
                    writeback(op, mode);
                    stX(op.rd, R::RAX, true);
                }
                break;
            }
            case K::PAIR: {
                const unsigned nb = f.a;
                const bool ld = f.b;
                const int mode = f.c;
                addrOf(op, mode, sl);
                fastAddr(!ld, 2 * nb, sl);
                if (ld) {
                    load(R::RAX, nb, 1, 0);
                    load(R::RDX, nb, 1, (int32_t)nb);
                    stX(op.rd, R::RAX, true);
                    stX(op.ra, R::RDX, true);
                    writeback(op, mode);
                } else {
                    ldX(R::RAX, op.rd);
                    ldX(R::RDX, op.ra);
                    store(R::RAX, nb, 0);
                    store(R::RDX, nb, (int32_t)nb);
                    writeback(op, mode);
                    if (!last) watchCheck(i + 1, at + 4);
                }
                break;
            }
        }
        if (!sl.empty()) slows.push_back({std::move(sl), &op, at, i, j.p});
    }
    if (!pcSet) stPc(pc + 4ull * n);
    j.movI(R::RAX, n);
    uint8_t* tail = j.p;
    j.b(0x41); j.b(0x5c); j.b(0x5d); j.b(0x5b); j.b(0xc3);  // pop r12, rbp, rbx; ret
    for (uint8_t* r : wrong) Jit::fix(r, j.p);
    j.aluRR(R::A_XOR, R::RAX, R::RAX, false);
    Jit::fix(j.jmp(), tail);
    // the slow ways: the decoded function, then on as after a call
    for (auto& sw : slows) {
        for (uint8_t* r : sw.rels) Jit::fix(r, j.p);
        stPc(sw.at + 4);
        j.movI(R::RAX, sw.at); stRaw(OCUR, R::RAX);
        callOp(*sw.op, sw.at);
        if (sw.i + 1 < n) {
            afterCall(sw.at, sw.i + 1);
            Jit::fix(j.jmp(), sw.back);
        } else {
            j.movI(R::RAX, n);
            Jit::fix(j.jmp(), tail);
        }
    }
    for (auto& e : exits) {
        Jit::fix(e.rel, j.p);
        if (e.setPc) stPc(e.pc);
        j.movI(R::RAX, e.count);
        Jit::fix(j.jmp(), tail);
    }
    j.used = (size_t)(j.p - j.buf);
    s.code = start;
    s.n = (uint16_t)n;
    j.compiled++;
    return true;
}
#pragma GCC diagnostic pop

// Runs the block that starts at instruction k of page cp (pc), translating
// it if it has become hot. False: none ran (the caller runs one instruction).
inline bool Machine::jitRun(Cpu& c, CodePage* cp, unsigned k, uint64_t pc, uint64_t end) {
    if (!jit_[c.id]) {
        jit_[c.id] = std::make_unique<Jit>();
        if (jit_[c.id]->failed) { jitOn_ = false; return false; }
    }
    Jit& j = *jit_[c.id];
    if (cp->jitGen != jitGen_[c.id] || !cp->jit) {
        cp->jit.reset(new JitSlot[1024]());
        cp->jitGen = jitGen_[c.id];
    }
    JitSlot& s = cp->jit[k];
    if (!s.code) {
        if (++s.hits != jitHot_) return false;
        if (j.used + (256u << 10) > Jit::BUF) {  // full: start again
            j.used = j.codeStart;
            jitGen_[c.id]++;
            cp->jit.reset(new JitSlot[1024]());
            cp->jitGen = jitGen_[c.id];
            return false;
        }
        if (!jitTranslate(c, cp, k, pc, s)) return false;
    }
    if (c.cycle + s.n > end) return false;
    // then the blocks that follow on the same page, while nothing the run
    // loop looks at between instructions has happened
    JitSlot* b = &s;
    for (unsigned hop = 0;; hop++) {
        const uint32_t ran = ((uint32_t (*)(Machine*, Cpu*))b->code)(this, &c);
        if (!ran) return hop > 0;  // made for another EL or SP
        c.cycle += ran;
        c.insns += ran;
        const uint64_t npc = c.pc;
        if (((npc ^ pc) >> 12) || (npc & 3) || hop >= 255) return true;
        if (exit_ || c.irqLine || c.wfi || c.off || WriteWatch::writes() != c.codeWrites || (mt && c.flushReq))
            return true;
        const Cpu::Fast& f = c.fastX[(npc >> 12) & (Cpu::FAST_N - 1)];
        if (f.key != ((npc >> 12) << 1 | (c.el == 0)) || f.code != cp || cp->jitGen != jitGen_[c.id]) return true;
        b = &cp->jit[(npc & 0xfff) >> 2];
        if (!b->code || c.cycle + b->n > end) return true;
    }
}
#endif

}  // namespace a64
}  // namespace vp
