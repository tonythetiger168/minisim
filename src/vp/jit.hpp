// jit.hpp - the pieces of mvplat's dynamic translator that do not depend on
// the machine: an x86-64 instruction emitter, the executable code buffer, the
// block tables, and the entry and exit sequences.
//
// Translated code works on the fast loop's register copy (r[0..32], r[32]
// the sink for x0) through rbx, keeps the instruction budget in r12 and the
// cached memory region in r13 (host pointer), r14 (guest base) and r15 (span
// for loads); rbp points to a JitCtx for everything else. A block starts by
// checking that the budget holds all of it; it leaves through the shared
// epilogue with the next pc in rax and, for an exit that may be linked to the
// next block, the address of the jump's rel32 field in rdx.
#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
#include <sys/mman.h>
#define MVPLAT_JIT 1
#else
#define MVPLAT_JIT 0
#endif

namespace vp {

// What translated code reads and writes besides registers; offsets are fixed
// (the emitter uses them).
struct JitCtx {
    uint64_t* regs;    // 0
    uint64_t left;     // 8: instructions still to run
    uint8_t* cp;       // 16: host address of the cached region
    uint64_t cb;       // 24: its guest base
    uint64_t span;     // 32: loads: ad - cb < span
    uint64_t pc;       // 40: where translated code left
    uint64_t link;     // 48: the rel32 to patch for the next block, or 0
    uint64_t sspan;    // 56: stores: ad - cb < sspan
    uint64_t th;       // 64: tohost (stores within 8 bytes of it leave)
    uint64_t reason;   // 72: 0 go on with the next block, 1 run the interpreter, 2 run the counting version
    void* jtab;        // 80: the indirect-jump table
};

struct Jit {
    static constexpr size_t BUF = 32u << 20;
    static constexpr unsigned JT = 1u << 16;  // indirect-jump table entries
    struct JtEntry { uint64_t pc; uint8_t* code; };
    uint8_t* buf = nullptr;
    size_t used = 0, codeStart = 0;
    uint8_t* epi = nullptr;  // the shared epilogue
    void (*enter)(JitCtx*, uint8_t*) = nullptr;
    JtEntry* jt = nullptr;
    std::unordered_map<uint64_t, uint8_t*> blocks;  // pc -> code
    std::unordered_map<uint64_t, uint8_t*> steps;   // pc -> the counting version
    uint32_t gen = 0;
    bool failed = false;
    uint64_t flushes = 0, compiled = 0;

    Jit() {
#if MVPLAT_JIT
        void* p = mmap(nullptr, BUF, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { failed = true; return; }
        buf = (uint8_t*)p;
        jt = new JtEntry[JT];
        for (unsigned i = 0; i < JT; i++) jt[i] = {~0ull, nullptr};
        prologue();
        flush();
#else
        failed = true;
#endif
    }
    ~Jit() {
#if MVPLAT_JIT
        if (buf) munmap(buf, BUF);
        delete[] jt;
#endif
    }
    Jit(const Jit&) = delete;
    Jit& operator=(const Jit&) = delete;

    std::vector<uint32_t> jtUsed;  // the table entries filled since the last flush
    void flush() {
        used = codeStart;
        blocks.clear();
        steps.clear();
        if (jtUsed.size() > JT / 8) for (unsigned i = 0; i < JT; i++) jt[i] = {~0ull, nullptr};
        else for (uint32_t i : jtUsed) jt[i] = {~0ull, nullptr};
        jtUsed.clear();
        flushes++;
    }
    uint8_t* find(uint64_t pc) {
        JtEntry& e = jt[(pc >> 1) & (JT - 1)];
        if (e.pc == pc) return e.code;
        auto it = blocks.find(pc);
        if (it == blocks.end()) return nullptr;
        if (e.pc == ~0ull) jtUsed.push_back((uint32_t)((pc >> 1) & (JT - 1)));
        e = {pc, it->second};
        return it->second;
    }
    void add(uint64_t pc, uint8_t* code) {
        blocks[pc] = code;
        JtEntry& e = jt[(pc >> 1) & (JT - 1)];
        if (e.pc == ~0ull) jtUsed.push_back((uint32_t)((pc >> 1) & (JT - 1)));
        e = {pc, code};
        compiled++;
    }
    static void patch(uint64_t site, uint8_t* to) {
        int32_t rel = (int32_t)(to - (uint8_t*)(site + 4));
        memcpy((void*)site, &rel, 4);
    }

    // ---------------------------------------------------------------- emitter
    enum Reg { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
    enum Cc { CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7, CC_L = 0xc, CC_GE = 0xd };
    enum Alu { A_ADD = 0, A_OR = 1, A_AND = 4, A_SUB = 5, A_XOR = 6, A_CMP = 7 };
    uint8_t* p = nullptr;  // where the emitter writes
    void b(uint8_t v) { *p++ = v; }
    void d32(uint32_t v) { memcpy(p, &v, 4); p += 4; }
    void q64(uint64_t v) { memcpy(p, &v, 8); p += 8; }
    void rex(bool w, int reg, int rm, int index = 0, bool force = false) {
        uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg >> 3) << 2) | ((index >> 3) << 1) | (rm >> 3));
        if (r != 0x40 || force) b(r);
    }
    // ModRM (and SIB, displacement) for [base + disp].
    void mem(int reg, int base, int32_t disp) {
        const int bb = base & 7;
        if (disp == 0 && bb != 5) {
            b((uint8_t)(((reg & 7) << 3) | bb));
            if (bb == 4) b(0x24);
        } else if (disp >= -128 && disp <= 127) {
            b((uint8_t)(0x40 | ((reg & 7) << 3) | bb));
            if (bb == 4) b(0x24);
            b((uint8_t)disp);
        } else {
            b((uint8_t)(0x80 | ((reg & 7) << 3) | bb));
            if (bb == 4) b(0x24);
            d32((uint32_t)disp);
        }
    }
    // ModRM, SIB for [base + index] (disp8 0, so r13 works as the base).
    void memIdx(int reg, int base, int index) {
        b((uint8_t)(0x44 | ((reg & 7) << 3)));
        b((uint8_t)(((index & 7) << 3) | (base & 7)));
        b(0);
    }
    void modrr(int reg, int rm) { b((uint8_t)(0xc0 | ((reg & 7) << 3) | (rm & 7))); }
    // op reg, [rbx + 8*g] and back: the guest registers.
    void ldg(int reg, unsigned g, bool w = true) { rex(w, reg, RBX); b(0x8b); mem(reg, RBX, (int32_t)(8 * g)); }
    void stg(unsigned g, int reg) { rex(true, reg, RBX); b(0x89); mem(reg, RBX, (int32_t)(8 * g)); }
    void aluG(Alu a, int reg, unsigned g, bool w = true) {
        static const uint8_t opc[8] = {0x03, 0x0b, 0, 0, 0x23, 0x2b, 0x33, 0x3b};
        rex(w, reg, RBX); b(opc[a]); mem(reg, RBX, (int32_t)(8 * g));
    }
    void imulG(int reg, unsigned g, bool w = true) { rex(w, reg, RBX); b(0x0f); b(0xaf); mem(reg, RBX, (int32_t)(8 * g)); }
    // op reg, [rbp + off]: the context.
    void aluC(Alu a, int reg, int32_t off) {
        static const uint8_t opc[8] = {0x03, 0x0b, 0, 0, 0x23, 0x2b, 0x33, 0x3b};
        rex(true, reg, RBP); b(opc[a]); mem(reg, RBP, off);
    }
    void aluRR(Alu a, int dst, int src, bool w = true) {  // dst op= src
        static const uint8_t opc[8] = {0x01, 0x09, 0, 0, 0x21, 0x29, 0x31, 0x39};
        rex(w, src, dst); b(opc[a]); modrr(src, dst);
    }
    void aluI(Alu a, int reg, int32_t imm, bool w = true) {
        rex(w, 0, reg);
        if (imm >= -128 && imm <= 127) { b(0x83); modrr(a, reg); b((uint8_t)imm); }
        else { b(0x81); modrr(a, reg); d32((uint32_t)imm); }
    }
    void movI(int reg, uint64_t v) {
        if ((int64_t)v == (int32_t)v) { rex(true, 0, reg); b(0xc7); modrr(0, reg); d32((uint32_t)v); }
        else if (v <= 0xffffffffu) { rex(false, 0, reg); b((uint8_t)(0xb8 + (reg & 7))); d32((uint32_t)v); }
        else { rex(true, 0, reg); b((uint8_t)(0xb8 + (reg & 7))); q64(v); }
    }
    void movRR(int dst, int src, bool w = true) { rex(w, src, dst); b(0x89); modrr(src, dst); }
    void shiftI(int ext, int reg, unsigned sh, bool w = true) { rex(w, 0, reg); b(0xc1); modrr(ext, reg); b((uint8_t)sh); }
    void shiftCl(int ext, int reg, bool w = true) { rex(w, 0, reg); b(0xd3); modrr(ext, reg); }
    void movsxd(int dst, int src) { rex(true, dst, src); b(0x63); modrr(dst, src); }
    void setcc(Cc c, int reg8) { rex(false, 0, reg8, 0, reg8 >= 4); b(0x0f); b((uint8_t)(0x90 + c)); modrr(0, reg8); }
    void movzxb(int dst, int src8) { rex(false, dst, src8, 0, src8 >= 4); b(0x0f); b(0xb6); modrr(dst, src8); }
    uint8_t* jcc(Cc c) { b(0x0f); b((uint8_t)(0x80 + c)); d32(0); return p - 4; }   // rel32 to fill
    uint8_t* jmp() { b(0xe9); d32(0); return p - 4; }
    static void fix(uint8_t* rel, const uint8_t* to) {
        int32_t v = (int32_t)(to - (rel + 4));
        memcpy(rel, &v, 4);
    }
    void stCtx(int32_t off, int reg) { rex(true, reg, RBP); b(0x89); mem(reg, RBP, off); }
    void ldCtx(int reg, int32_t off) { rex(true, reg, RBP); b(0x8b); mem(reg, RBP, off); }
    void movCtxI(int32_t off, int32_t v) { rex(true, 0, RBP); b(0xc7); mem(0, RBP, off); d32((uint32_t)v); }

private:
    // enter(ctx = rdi, code = rsi): save the callee-saved registers, load the
    // pinned ones, jump to the block. The epilogue (pc in rax, link in rdx)
    // stores them back and returns from enter.
    void prologue() {
        p = buf;
        enter = (void (*)(JitCtx*, uint8_t*))p;
        for (int r : {RBX, RBP, R12, R13, R14, R15}) { rex(false, 0, r); b((uint8_t)(0x50 + (r & 7))); }
        movRR(RBP, RDI);
        ldCtx(RBX, 0); ldCtx(R12, 8); ldCtx(R13, 16); ldCtx(R14, 24); ldCtx(R15, 32);
        rex(false, 0, RSI); b(0xff); modrr(4, RSI);  // jmp rsi
        epi = p;
        stCtx(8, R12); stCtx(40, RAX); stCtx(48, RDX);
        for (int r : {R15, R14, R13, R12, RBP, RBX}) { rex(false, 0, r); b((uint8_t)(0x58 + (r & 7))); }
        b(0xc3);
        codeStart = (size_t)(p - buf);
    }
};

}  // namespace vp
