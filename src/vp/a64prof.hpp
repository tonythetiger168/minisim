// a64prof.hpp - what mvplat counts on an AArch64 processor while profiling.
//
// The engine only counts; the report (-profile) is the extension library's.
// Every instruction is counted by class, at EL0 and EL1 apart, and every
// PROF_PERIOD-th instruction's address is kept (sampled hot spots: a
// hash-map update on every instruction would slow a run down several
// times). Exceptions are counted by kind and, for synchronous ones, by
// exception class (ESR_EL1.EC), so system calls (SVC) and page faults
// (aborts) show up.
//
// The classes follow the A64 encoding groups of DDI 0487 (C4.1, "A64
// instruction set encoding"): op0 = bits 28:25 picks data processing
// (immediate), branches/exceptions/system, loads and stores, data
// processing (register) and SIMD/floating point; each is split further by
// its own sub-group fields.
#pragma once

#include <cstdint>
#include <unordered_map>

namespace vp {
namespace a64 {

enum ProfClass : uint8_t {
    PC_ADR, PC_ADDSUB_IMM, PC_LOGIC_IMM, PC_MOVE_WIDE, PC_BITFIELD, PC_EXTRACT,
    PC_B, PC_BCOND, PC_CB_TB, PC_BR, PC_EXC, PC_SYSTEM,
    PC_LOAD, PC_STORE, PC_LOAD_PAIR, PC_STORE_PAIR, PC_LOAD_LIT, PC_EXCLUSIVE, PC_ATOMIC,
    PC_SIMD_LOAD, PC_SIMD_STORE,
    PC_ADDSUB_REG, PC_LOGIC_REG, PC_ADC, PC_CCMP, PC_CSEL, PC_DP1, PC_DP2, PC_MUL, PC_DIV, PC_CRC,
    PC_FP, PC_SIMD, PC_OTHER,
    PC_N
};

inline const char* profClassName(unsigned c) {
    static const char* const n[PC_N] = {
        "adr/adrp", "add/sub imm", "logical imm", "move wide", "bitfield", "extract",
        "b/bl", "b.cond", "cbz/tbz", "br/blr/ret", "svc/hvc/brk", "system",
        "load", "store", "load pair", "store pair", "load literal", "exclusive", "atomic (LSE)",
        "simd/fp load", "simd/fp store",
        "add/sub reg", "logical reg", "adc/sbc", "ccmp", "csel", "dp 1-source", "dp 2-source", "multiply", "divide",
        "crc32",
        "fp scalar", "simd vector", "other"};
    return c < PC_N ? n[c] : "?";
}

// The class of one A64 instruction.
inline ProfClass profClass(uint32_t i) {
    const uint32_t op0 = i >> 25 & 0xf;
    if ((op0 & 0xe) == 0x8) {  // data processing, immediate
        switch (i >> 23 & 7) {
        case 0: case 1: return PC_ADR;
        case 2: case 3: return PC_ADDSUB_IMM;
        case 4: return PC_LOGIC_IMM;
        case 5: return PC_MOVE_WIDE;
        case 6: return PC_BITFIELD;
        default: return PC_EXTRACT;
        }
    }
    if ((op0 & 0xe) == 0xa) {  // branches, exception generation, system
        const uint32_t hi = i >> 29;
        if ((hi & 3) == 0) return PC_B;                                    // B, BL
        if ((hi & 3) == 1) return PC_CB_TB;                                // CBZ/CBNZ, TBZ/TBNZ
        if (hi == 2) return PC_BCOND;
        if (hi == 6) {
            if ((i >> 24 & 3) == 0) return PC_EXC;                         // SVC, HVC, SMC, BRK
            if ((i & 0xffc00000u) == 0xd5000000u) return PC_SYSTEM;       // MSR, MRS, barriers, hints
            if (i >> 25 & 1) return PC_BR;                                 // BR, BLR, RET, ERET
        }
        return PC_OTHER;
    }
    if ((op0 & 5) == 4) {  // loads and stores
        const bool v = i >> 26 & 1, l = i >> 22 & 1;
        if ((i & 0xbf000000u) == 0x0c000000u || (i & 0xbf800000u) == 0x0d000000u ||
            (i & 0xbf800000u) == 0x0d800000u)  // SIMD structures (LD1-LD4, ST1-ST4)
            return l ? PC_SIMD_LOAD : PC_SIMD_STORE;
        if ((i & 0x3f000000u) == 0x08000000u) return PC_EXCLUSIVE;         // exclusives, acquire/release, CAS
        if ((i & 0x3b000000u) == 0x18000000u) return PC_LOAD_LIT;
        if (v) return l ? PC_SIMD_LOAD : PC_SIMD_STORE;  // L (or opc bit 0) is bit 22 in every form
        if ((i & 0x3a000000u) == 0x28000000u) return l ? PC_LOAD_PAIR : PC_STORE_PAIR;
        if ((i & 0x3b200c00u) == 0x38200000u) return PC_ATOMIC;           // LDADD.., SWP
        if ((i & 0x3a000000u) == 0x38000000u) {
            const uint32_t opc = i >> 22 & 3;
            if ((i >> 30) == 3 && opc == 2) return PC_OTHER;               // PRFM
            return opc == 0 ? PC_STORE : PC_LOAD;
        }
        return PC_OTHER;
    }
    if ((op0 & 7) == 5) {  // data processing, register
        if (!(i >> 28 & 1)) return (i >> 24 & 1) ? PC_ADDSUB_REG : PC_LOGIC_REG;
        if (i >> 24 & 1) return PC_MUL;                                    // 3-source
        switch (i >> 21 & 7) {
        case 0: return PC_ADC;
        case 2: return PC_CCMP;
        case 4: return PC_CSEL;
        case 6: {
            if (i >> 30 & 1) return PC_DP1;
            const uint32_t opc = i >> 10 & 0x3f;
            if ((opc & 0x3e) == 2) return PC_DIV;
            if ((opc & 0x38) == 0x10) return PC_CRC;
            return PC_DP2;
        }
        default: return PC_OTHER;
        }
    }
    if ((op0 & 7) == 7) return ((i >> 28 & 1) && !(i >> 30 & 1)) ? PC_FP : PC_SIMD;
    return PC_OTHER;
}

constexpr unsigned PROF_PERIOD = 97;  // a prime, so loops do not alias with it

// One processor's counts.
struct Prof {
    uint64_t cls[2][PC_N] = {};          // [EL0, EL1][class]
    uint64_t exc[2][64 + 4] = {};        // [taken from EL0, EL1][EC, or 64 + IRQ/FIQ/SError kind]
    std::unordered_map<uint64_t, uint64_t> pcs[2];  // sampled addresses, [EL0, EL1]
    uint32_t left = PROF_PERIOD;
    uint64_t total() const {
        uint64_t n = 0;
        for (auto& e : cls) for (uint64_t v : e) n += v;
        return n;
    }
};

}  // namespace a64
}  // namespace vp
