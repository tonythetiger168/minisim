// rv32.hpp - the RISC-V hart: RV32IMAC or RV64IMAC with Zicsr, Zifencei,
// machine, supervisor and user modes, and the compressed-instruction expander.
//
// Registers and CSRs are 64 bits wide. An RV32 hart keeps every register
// sign-extended from bit 31 (as an RV64 hart running RV32 code would), so
// most instructions need no separate 32-bit form.
//
// The hart's state is plain data; the instruction loop that runs it is in
// machine.hpp, because memory access belongs to the platform (devices, the
// per-hart write buffers of a parallel round, watchpoints, HTIF).
#pragma once

#include <memory>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace vp {

// Per-hart write buffer for a parallel round: a hart's stores land here and
// its loads see them, while every other hart sees memory as it was when the
// round began. At the end of the round the buffers are written back in hart
// order, so the result does not depend on how the harts were scheduled onto
// threads.
struct Overlay {
    struct Page {
        uint8_t d[4096];
        uint64_t m[64];   // bit i: d[i] holds a byte this hart wrote
        uint8_t* base;    // the page in the device's memory
        bool has(unsigned i) const { return (m[i >> 6] >> (i & 63)) & 1; }
        void set(unsigned i) { m[i >> 6] |= 1ull << (i & 63); }
    };
    std::unordered_map<uint64_t, Page*> map;
    std::vector<std::pair<uint64_t, Page*>> dirty;  // in the order first written
    std::vector<Page*> spare;
    // A direct-mapped cache in front of the map, holding misses as well.
    uint64_t tag[32];
    Page* ptr[32];
    Overlay() { clearCache(); }
    void clearCache() { for (auto& t : tag) t = ~0ull; }

    ~Overlay() {
        for (auto& [k, p] : map) delete p;
        for (Page* p : spare) delete p;
    }
    bool empty() const { return dirty.empty(); }
    Page* find(uint64_t pg) {
        unsigned i = pg & 31;
        if (tag[i] == pg) return ptr[i];
        auto it = map.find(pg);
        Page* p = it == map.end() ? nullptr : it->second;
        tag[i] = pg;
        ptr[i] = p;
        return p;
    }
    Page* touch(uint64_t pg, uint8_t* base) {
        Page* p = find(pg);
        if (p) return p;
        if (!spare.empty()) { p = spare.back(); spare.pop_back(); }
        else p = new Page;
        memset(p->m, 0, sizeof p->m);
        p->base = base;
        map[pg] = p;
        dirty.emplace_back(pg, p);
        tag[pg & 31] = pg;
        ptr[pg & 31] = p;
        return p;
    }
    // Write the buffer back and empty it. Returns the pages written, so the
    // caller can break other harts' LR reservations on them.
    template<typename F>
    void commit(F&& onPage) {
        for (auto& [pg, p] : dirty) {
            for (unsigned w = 0; w < 64; w++) {
                uint64_t bits = p->m[w];
                if (bits == ~0ull) { memcpy(p->base + 64 * w, p->d + 64 * w, 64); continue; }
                while (bits) {
                    unsigned b = (unsigned)__builtin_ctzll(bits);
                    bits &= bits - 1;
                    p->base[64 * w + b] = p->d[64 * w + b];
                }
            }
            onPage(pg);
            spare.push_back(p);
        }
        dirty.clear();
        map.clear();
        clearCache();
    }
};

enum class Stop : uint8_t {
    Budget,   // reached the end of its cycle budget
    Pending,  // stopped before an access that must wait for the ordered phase
    Break,    // a breakpoint, a watchpoint or a single step (for the debugger)
    Exit      // the platform asked to stop
};

// mstatus bits mvplat implements.
enum : uint32_t {
    MSTATUS_MIE = 1u << 3,
    MSTATUS_MPIE = 1u << 7,
    MSTATUS_MPP = 3u << 11,
    MSTATUS_MPRV = 1u << 17,
    MSTATUS_TW = 1u << 21,
    MSTATUS_SIE = 1u << 1,
    MSTATUS_SPIE = 1u << 5,
    MSTATUS_SPP = 1u << 8,
    MSTATUS_SUM = 1u << 18,
    MSTATUS_MXR = 1u << 19,
    MSTATUS_TVM = 1u << 20,
    MSTATUS_TSR = 1u << 22,
    MSTATUS_FS = 3u << 13,
    MSTATUS_VS = 3u << 9,
    // The view of mstatus that sstatus is.
    SSTATUS_MASK = (1u << 1) | (1u << 5) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 13) | (1u << 14) | (1u << 18) | (1u << 19),
};
enum : uint32_t { MIP_MSIP = 1u << 3, MIP_MTIP = 1u << 7, MIP_MEIP = 1u << 11,
                  MIP_SSIP = 1u << 1, MIP_STIP = 1u << 5, MIP_SEIP = 1u << 9 };

// vtype as a hart holds it (vill is a flag of its own): SEW in bits, log2
// LMUL from -3 to 3, and the tail and mask agnostic bits.
struct VType {
    unsigned sew = 8;
    int lmul = 0;
    bool ta = false, ma = false;
};

struct Hart {
    unsigned id = 0;
    bool rv64 = false;
    uint64_t x[32] = {};
    // F and D: the floating-point registers (a single is NaN-boxed: the
    // upper 32 bits all ones) and fcsr (frm << 5 | fflags).
    uint64_t f[32] = {};
    uint32_t fcsr = 0;
    // V: VLEN bits per register (up to VLEN_MAX), one array so that a
    // register group is contiguous; vl, vstart, vtype, vxrm, vxsat.
    static constexpr unsigned VLEN_MAX = 1024;
    unsigned vlen = 128;
    alignas(16) uint8_t vr[32 * VLEN_MAX / 8] = {};
    uint64_t vl = 0, vstart = 0, vtypeRaw = 0;
    VType vt;
    bool vill = true;
    uint8_t vxrm = 0, vxsat = 0;
    uint64_t pc = 0;
    uint64_t cycle = 0, instret = 0;
    int64_t cycleAdj = 0, instretAdj = 0;  // what software wrote to mcycle / minstret
    uint8_t priv = 3;
    uint64_t mstatus = 0, mtvec = 0, mscratch = 0, mepc = 0, mcause = 0, mtval = 0;
    uint32_t mie = 0, mip = 0, mcounteren = 0, mcountinhibit = 0;
    uint8_t pmpcfg[16] = {};
    uint64_t pmpaddr[16] = {};
    // Supervisor mode.
    uint64_t stvec = 0, sscratch = 0, sepc = 0, scause = 0, stval = 0, satp = 0;
    uint32_t scounteren = 0;
    uint32_t medeleg = 0, mideleg = 0;
    uint32_t mipSw = 0;  // SSIP, STIP, SEIP as software wrote them
    // Is paging on (Sv32 on RV32; Sv39, Sv48 or Sv57 on RV64)?
    bool paging() const { return rv64 ? (satp >> 60) != 0 : (satp >> 31) != 0; }
    // Values as the hart's XLEN sees them: sign-extended from bit 31 on RV32.
    uint64_t sx(uint64_t v) const { return rv64 ? v : (uint64_t)(int64_t)(int32_t)v; }
    // Addresses and the pc: zero-extended from bit 31 on RV32.
    uint64_t ax(uint64_t v) const { return rv64 ? v : (uint32_t)v; }
    // Translations, by virtual page (direct mapped); flushed by SFENCE.VMA
    // and by a write to satp.
    struct Tlb { uint64_t vpn, ppn; uint8_t pte; bool valid; };
    static constexpr unsigned TLB_SIZE = 256;
    Tlb tlb[TLB_SIZE] = {};
    void flushTlb() {
        for (auto& t : tlb) t.valid = false;
        fpVpn = ~0ull;
        flushDataPages();
    }
    // Pages of plain memory that loads (dl) and stores (ds) with paging on
    // last went to: va >> 12, where the page is in host memory, and the
    // privilege, SUM and MXR they were checked with. Each one was translated
    // (with A, and D for a store, set) and found allowed by PMP as a whole.
    // Emptied with the TLB and by PMP writes.
    struct DataPage { uint64_t vpn = ~0ull; uint8_t* host = nullptr; uint32_t ctx = 0; };
    static constexpr unsigned DP_SIZE = 64;
    DataPage dl[DP_SIZE], ds[DP_SIZE];
    void flushDataPages() {
        for (auto& e : dl) e.vpn = ~0ull;
        for (auto& e : ds) e.vpn = ~0ull;
    }
    // The page instructions were last fetched from, translated and found
    // executable by PMP as a whole: va >> 12, the physical page, and the
    // privilege it was checked at. Emptied with the TLB and by PMP writes.
    uint64_t fpVpn = ~0ull, fpPa = 0;
    unsigned fpPriv = 0;
    // The active PMP entries, decoded from the CSRs in priority order
    // (Machine::pmpDecode). pmpLock: some entry binds machine mode too.
    struct PmpRegion { uint64_t lo, hi; uint8_t cfg; };
    PmpRegion pmpR[16] = {};
    uint8_t pmpN = 0;
    bool pmpLock = false;
    bool wfi = false;
    bool resv = false;
    uint64_t resvAddr = 0;

    // Run state.
    bool par = false;       // in the parallel phase of a round
    Stop stop = Stop::Budget;
    bool stepping = false;  // the debugger asked for one instruction
    bool skipBp = false;    // resume over the breakpoint at pc
    int stopSig = 0;        // signal the debugger reports (5 = SIGTRAP)
    uint64_t watchAddr = 0; // the address a watchpoint hit
    int watchKind = 0;      // 0 none, 2 write, 3 read, 4 access (gdb Z types)
    Overlay ov;

    // Decoded instructions by pc (direct mapped). Instruction fetch sees
    // stores to code only after FENCE.I, as the ISA allows; loading a
    // program, a debugger write and a restore empty it too.
    struct Decoded {
        uint64_t pc;
        uint32_t insn, raw;
        int32_t immI;
        uint8_t len, op, rd, f3, rs1, rs2, f7;
        bool xlat;  // fetched through the MMU (the same pc means another page bare)
    };
    static constexpr unsigned DC_SIZE = 8192;
    std::unique_ptr<Decoded[]> dc{new Decoded[DC_SIZE]};
    Hart() { flushDecoded(); }
    void flushDecoded() { for (unsigned i = 0; i < DC_SIZE; i++) dc[i].pc = 1; }

    // The last plain memory device used, for the fast path.
    uint64_t cBase = 1, cEnd = 0;
    uint8_t* cPtr = nullptr;
    bool cRw = false;

    // -trace: the current instruction's line, then its effects.
    std::string traceBuf;
    char traceHead[64] = {};
    // -profile.
    uint64_t nClass[12] = {};
    std::unordered_map<uint64_t, uint64_t> pcHist;
};

// Instruction classes for -profile.
enum IClass { C_ALU, C_MUL, C_DIV, C_LOAD, C_STORE, C_BRANCH, C_JUMP, C_AMO, C_CSR, C_SYSTEM, C_FENCE, C_N };
inline const char* className(int c) {
    static const char* n[] = {"alu", "mul", "div", "load", "store", "branch", "jump", "atomic", "csr", "system", "fence"};
    return n[c];
}

// ----------------------------------------------------------------- C
// A compressed instruction becomes the 32-bit instruction it stands for, so
// one decoder runs both. Returns 0 for a reserved or illegal encoding.
namespace rvc {
inline uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
inline uint32_t I(uint32_t op, uint32_t rd, uint32_t f3, uint32_t rs1, int32_t imm) {
    return ((uint32_t)imm & 0xfff) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
inline uint32_t S(uint32_t op, uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm) {
    uint32_t u = (uint32_t)imm;
    return bits(u, 11, 5) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | bits(u, 4, 0) << 7 | op;
}
inline uint32_t B(uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm) {
    uint32_t u = (uint32_t)imm;
    return bits(u, 12, 12) << 31 | bits(u, 10, 5) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 |
           bits(u, 4, 1) << 8 | bits(u, 11, 11) << 7 | 0x63;
}
inline uint32_t J(uint32_t rd, int32_t imm) {
    uint32_t u = (uint32_t)imm;
    return bits(u, 20, 20) << 31 | bits(u, 10, 1) << 21 | bits(u, 11, 11) << 20 | bits(u, 19, 12) << 12 |
           rd << 7 | 0x6f;
}
inline uint32_t R(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd) {
    return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | 0x33;
}
inline int32_t sext(uint32_t v, int b) { return (int32_t)(v << (32 - b)) >> (32 - b); }

inline uint32_t expand(uint32_t c, bool rv64) {
    uint32_t op = c & 3, f3 = bits(c, 15, 13);
    uint32_t rdp = bits(c, 4, 2) + 8, rs1p = bits(c, 9, 7) + 8;
    uint32_t rd = bits(c, 11, 7), rs2 = bits(c, 6, 2);
    if (c == 0) return 0;
    if (op == 0) {
        switch (f3) {
            case 0: {  // C.ADDI4SPN
                uint32_t imm = bits(c, 12, 11) << 4 | bits(c, 10, 7) << 6 | bits(c, 6, 6) << 2 | bits(c, 5, 5) << 3;
                if (!imm) return 0;
                return I(0x13, rdp, 0, 2, (int32_t)imm);
            }
            case 2: {  // C.LW
                uint32_t imm = bits(c, 12, 10) << 3 | bits(c, 6, 6) << 2 | bits(c, 5, 5) << 6;
                return I(0x03, rdp, 2, rs1p, (int32_t)imm);
            }
            case 6: {  // C.SW
                uint32_t imm = bits(c, 12, 10) << 3 | bits(c, 6, 6) << 2 | bits(c, 5, 5) << 6;
                return S(0x23, 2, rs1p, rdp, (int32_t)imm);
            }
            case 3:
            case 7: {
                if (!rv64) {  // C.FLW, C.FSW (RV32)
                    uint32_t imm = bits(c, 12, 10) << 3 | bits(c, 6, 6) << 2 | bits(c, 5, 5) << 6;
                    return f3 == 3 ? I(0x07, rdp, 2, rs1p, (int32_t)imm) : S(0x27, 2, rs1p, rdp, (int32_t)imm);
                }
                // C.LD, C.SD (RV64)
                uint32_t imm = bits(c, 12, 10) << 3 | bits(c, 6, 5) << 6;
                return f3 == 3 ? I(0x03, rdp, 3, rs1p, (int32_t)imm) : S(0x23, 3, rs1p, rdp, (int32_t)imm);
            }
            case 1:
            case 5: {  // C.FLD, C.FSD
                uint32_t imm = bits(c, 12, 10) << 3 | bits(c, 6, 5) << 6;
                return f3 == 1 ? I(0x07, rdp, 3, rs1p, (int32_t)imm) : S(0x27, 3, rs1p, rdp, (int32_t)imm);
            }
            default: return 0;
        }
    }
    if (op == 1) {
        int32_t imm6 = sext(bits(c, 12, 12) << 5 | bits(c, 6, 2), 6);
        switch (f3) {
            case 0: return I(0x13, rd, 0, rd, imm6);  // C.ADDI / C.NOP
            case 1:
                if (rv64) {  // C.ADDIW
                    if (!rd) return 0;
                    return I(0x1b, rd, 0, rd, imm6);
                }
                [[fallthrough]];
            case 5: {  // C.JAL (RV32), C.J
                uint32_t u = bits(c, 12, 12) << 11 | bits(c, 11, 11) << 4 | bits(c, 10, 9) << 8 |
                             bits(c, 8, 8) << 10 | bits(c, 7, 7) << 6 | bits(c, 6, 6) << 7 |
                             bits(c, 5, 3) << 1 | bits(c, 2, 2) << 5;
                return J(f3 == 1 ? 1 : 0, sext(u, 12));
            }
            case 2: return I(0x13, rd, 0, 0, imm6);  // C.LI
            case 3:
                if (rd == 2) {  // C.ADDI16SP
                    uint32_t u = bits(c, 12, 12) << 9 | bits(c, 6, 6) << 4 | bits(c, 5, 5) << 6 |
                                 bits(c, 4, 3) << 7 | bits(c, 2, 2) << 5;
                    if (!u) return 0;
                    return I(0x13, 2, 0, 2, sext(u, 10));
                } else {  // C.LUI
                    if (!imm6) return 0;
                    return ((uint32_t)imm6 << 12 & 0xfffff000u) | rd << 7 | 0x37;
                }
            case 4: {
                uint32_t f2 = bits(c, 11, 10);
                if (f2 == 0 || f2 == 1) {  // C.SRLI, C.SRAI
                    if (bits(c, 12, 12) && !rv64) return 0;
                    uint32_t sh = bits(c, 12, 12) << 5 | bits(c, 6, 2);
                    return I(0x13, rs1p, 5, rs1p, (int32_t)(sh | (f2 == 1 ? 0x400 : 0)));
                }
                if (f2 == 2) return I(0x13, rs1p, 7, rs1p, imm6);  // C.ANDI
                if (bits(c, 12, 12)) {
                    if (!rv64) return 0;
                    switch (bits(c, 6, 5)) {
                        case 0: return R(0x20, rdp, rs1p, 0, rs1p) ^ 0x33 ^ 0x3b;  // C.SUBW
                        case 1: return R(0, rdp, rs1p, 0, rs1p) ^ 0x33 ^ 0x3b;     // C.ADDW
                        default: return 0;
                    }
                }
                switch (bits(c, 6, 5)) {
                    case 0: return R(0x20, rdp, rs1p, 0, rs1p);  // C.SUB
                    case 1: return R(0, rdp, rs1p, 4, rs1p);     // C.XOR
                    case 2: return R(0, rdp, rs1p, 6, rs1p);     // C.OR
                    default: return R(0, rdp, rs1p, 7, rs1p);    // C.AND
                }
            }
            case 6:
            case 7: {  // C.BEQZ, C.BNEZ
                uint32_t u = bits(c, 12, 12) << 8 | bits(c, 11, 10) << 3 | bits(c, 6, 5) << 6 |
                             bits(c, 4, 3) << 1 | bits(c, 2, 2) << 5;
                return B(f3 == 6 ? 0 : 1, rs1p, 0, sext(u, 9));
            }
        }
        return 0;
    }
    if (op == 2) {
        switch (f3) {
            case 0:  // C.SLLI
                if (bits(c, 12, 12) && !rv64) return 0;
                return I(0x13, rd, 1, rd, (int32_t)(bits(c, 12, 12) << 5 | bits(c, 6, 2)));
            case 2: {  // C.LWSP
                if (!rd) return 0;
                uint32_t u = bits(c, 12, 12) << 5 | bits(c, 6, 4) << 2 | bits(c, 3, 2) << 6;
                return I(0x03, rd, 2, 2, (int32_t)u);
            }
            case 4:
                if (!bits(c, 12, 12)) {
                    if (!rs2) { if (!rd) return 0; return I(0x67, 0, 0, rd, 0); }  // C.JR
                    return R(0, rs2, 0, 0, rd);                                   // C.MV
                }
                if (!rd && !rs2) return 0x00100073;                               // C.EBREAK
                if (!rs2) return I(0x67, 1, 0, rd, 0);                            // C.JALR
                return R(0, rs2, rd, 0, rd);                                      // C.ADD
            case 6: {  // C.SWSP
                uint32_t u = bits(c, 12, 9) << 2 | bits(c, 8, 7) << 6;
                return S(0x23, 2, 2, rs2, (int32_t)u);
            }
            case 1: {  // C.FLDSP
                uint32_t u = bits(c, 12, 12) << 5 | bits(c, 6, 5) << 3 | bits(c, 4, 2) << 6;
                return I(0x07, rd, 3, 2, (int32_t)u);
            }
            case 5: {  // C.FSDSP
                uint32_t u = bits(c, 12, 10) << 3 | bits(c, 9, 7) << 6;
                return S(0x27, 3, 2, rs2, (int32_t)u);
            }
            case 3: {  // C.LDSP (RV64), C.FLWSP (RV32)
                if (!rv64) {
                    uint32_t u = bits(c, 12, 12) << 5 | bits(c, 6, 4) << 2 | bits(c, 3, 2) << 6;
                    return I(0x07, rd, 2, 2, (int32_t)u);
                }
                if (!rd) return 0;
                uint32_t u = bits(c, 12, 12) << 5 | bits(c, 6, 5) << 3 | bits(c, 4, 2) << 6;
                return I(0x03, rd, 3, 2, (int32_t)u);
            }
            case 7: {  // C.SDSP (RV64), C.FSWSP (RV32)
                if (!rv64) {
                    uint32_t u = bits(c, 12, 9) << 2 | bits(c, 8, 7) << 6;
                    return S(0x27, 2, 2, rs2, (int32_t)u);
                }
                uint32_t u = bits(c, 12, 10) << 3 | bits(c, 9, 7) << 6;
                return S(0x23, 3, 2, rs2, (int32_t)u);
            }
            default: return 0;
        }
    }
    return 0;
}
}  // namespace rvc

}  // namespace vp
