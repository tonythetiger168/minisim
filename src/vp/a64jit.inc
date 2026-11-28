// a64jit.inc - blocks of AArch64 instructions translated to x86-64 code.
// Part of a64machine.hpp.
//
// A block starts where the run loop found an instruction that has started
// blocks often (JIT_HOT times), on a decoded page (a64dec.inc), and runs on
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
    const int32_t OFF_PC = (int32_t)offsetof(Cpu, pc), OFF_CUR = (int32_t)offsetof(Cpu, curPc),
                  OFF_CW = (int32_t)offsetof(Cpu, codeWrites);
    const int32_t OFF_EXIT = (int32_t)offsetof(Machine, exit_);
    using R = Jit;
    j.p = j.buf + j.used;
    uint8_t* start = j.p;
    // uint32_t block(Machine* rdi, Cpu* rsi): rbx = the processor, rbp = the
    // machine, r12 = the write counter's address (3 pushes: rsp 16-aligned)
    j.b(0x53); j.b(0x55); j.b(0x41); j.b(0x54);
    j.movRR(R::RBX, R::RSI);
    j.movRR(R::RBP, R::RDI);
    j.movI(R::R12, (uint64_t)(uintptr_t)WriteWatch::writesPtr());
    std::vector<std::pair<uint8_t*, unsigned>> exits;
    for (unsigned i = 0; i < n; i++) {
        const Op& op = cp->op[k + i];
        const uint64_t at = pc + 4ull * i;
        j.movI(R::RAX, at + 4);
        j.rex(true, R::RAX, R::RBX); j.b(0x89); j.mem(R::RAX, R::RBX, OFF_PC);   // c.pc = at + 4
        j.movI(R::RAX, at);
        j.rex(true, R::RAX, R::RBX); j.b(0x89); j.mem(R::RAX, R::RBX, OFF_CUR);  // c.curPc = at
        j.movRR(R::RDI, R::RBP);
        j.movRR(R::RSI, R::RBX);
        j.movI(R::RDX, (uint64_t)(uintptr_t)&op);
        j.movI(R::RCX, at);
        j.movI(R::RAX, (uint64_t)(uintptr_t)op.fn);
        j.rex(false, 0, R::RAX); j.b(0xff); j.modrr(2, R::RAX);                    // call rax
        if (i + 1 == n) break;
        // went on to the next instruction?
        j.movI(R::RAX, at + 4);
        j.rex(true, R::RAX, R::RBX); j.b(0x3b); j.mem(R::RAX, R::RBX, OFF_PC);   // cmp rax, [rbx + pc]
        exits.emplace_back(j.jcc(R::CC_NE), i + 1);
        // a stop asked for?
        j.rex(false, 0, R::RBP); j.b(0x80); j.mem(7, R::RBP, OFF_EXIT); j.b(0);  // cmp byte [rbp + exit_], 0
        exits.emplace_back(j.jcc(R::CC_NE), i + 1);
        // a watched page written?
        j.rex(true, R::RAX, R::R12); j.b(0x8b); j.mem(R::RAX, R::R12, 0);        // mov rax, [r12]
        j.rex(true, R::RAX, R::RBX); j.b(0x3b); j.mem(R::RAX, R::RBX, OFF_CW);   // cmp rax, [rbx + codeWrites]
        exits.emplace_back(j.jcc(R::CC_NE), i + 1);
    }
    j.movI(R::RAX, n);
    uint8_t* tail = j.p;
    j.b(0x41); j.b(0x5c); j.b(0x5d); j.b(0x5b); j.b(0xc3);  // pop r12, rbp, rbx; ret
    for (auto& [rel, cnt] : exits) {
        Jit::fix(rel, j.p);
        j.movI(R::RAX, cnt);
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
        if (++s.hits != JIT_HOT) return false;
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
    const uint32_t ran = ((uint32_t (*)(Machine*, Cpu*))s.code)(this, &c);
    c.cycle += ran;
    c.insns += ran;
    return true;
}
#endif

}  // namespace a64
}  // namespace vp
