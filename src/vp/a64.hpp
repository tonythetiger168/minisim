// a64.hpp - an AArch64 processor element (Armv8-A, EL0 and EL1) and the
// platform it runs on, for mvplat. Written from the Arm Architecture Reference
// Manual for A-profile (DDI 0487): the A64 instruction set (chapter C4 has
// the encoding index this decoder follows), the AArch64 exception model (D1),
// the VMSAv8-64 translation (D8: stage 1, 4 KiB granule) and the generic
// timer (D11). No Arm code is used and no Arm core is claimed: MIDR_EL1 has
// implementer 0 (software use).
//
// What is there: the integer instructions, loads and stores (exclusives,
// load-acquire / store-release, pairs, all addressing modes), branches, the
// system instructions Linux uses (MRS / MSR, DC ZVA, IC, TLBI, AT, the
// barriers, WFI / WFE), SVC / HVC / BRK / ERET, the SIMD and floating-point
// registers with their loads and stores and the AdvSIMD and scalar
// floating-point instructions in a64simd.inc. EL2 and EL3 are not there:
// HVC and SMC from EL1 reach the platform's firmware (PSCI, SMCCC), as on a
// board whose firmware sits above the operating system.
//
// The interrupt controller is a GICv3 with its CPU interface in system
// registers (a64gic.hpp); the timer's interrupts are its PPIs 30 (EL1
// physical) and 27 (virtual).
#pragma once

#include <atomic>
#include <memory>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "bus.hpp"

#include "a64prof.hpp"

namespace vp {
namespace a64 {

using u128 = unsigned __int128;

// Exception classes (ESR_ELx.EC, D23.2.41).
enum : uint32_t {
    EC_UNKNOWN = 0x00, EC_WFX = 0x01, EC_FP = 0x07, EC_ILLEGAL = 0x0e, EC_SVC = 0x15, EC_HVC = 0x16,
    EC_SMC = 0x17, EC_SYS = 0x18, EC_IABT_LOW = 0x20, EC_IABT_CUR = 0x21, EC_PC_ALIGN = 0x22,
    EC_DABT_LOW = 0x24, EC_DABT_CUR = 0x25, EC_SP_ALIGN = 0x26, EC_FP_EXC = 0x2c, EC_BRK = 0x3c,
};

// A system register by its encoding (op0, op1, CRn, CRm, op2), as MRS / MSR
// carry it in bits 20:5.
constexpr uint32_t SR(unsigned op0, unsigned op1, unsigned crn, unsigned crm, unsigned op2) {
    return op0 << 14 | op1 << 11 | crn << 7 | crm << 3 | op2;
}

class Machine;
struct Cpu;

// A decoded instruction (a64dec.inc): what runs it, and its fields.
struct Op {
    using Fn = void (*)(Machine&, Cpu&, const Op&, uint64_t pc);
    Fn fn;
    uint32_t insn;
    uint8_t rd, rn, rm, ra;
    uint64_t imm, imm2;
};
// The instructions of a 4 KiB page of RAM, decoded when first run.
struct CodePage {
    uint8_t* host;  // the page in host memory
    uint64_t pa;
    int wr;         // its WriteWatch region (-2: read-only memory)
    uint32_t seen;  // the page's write count when it was decoded
    Op op[1024];
};

struct Cpu {
    unsigned id = 0;
    uint64_t x[32] = {};      // x[31] reads as zero (XZR); the stack pointers are sp[]
    uint64_t sp[2] = {};      // SP_EL0, SP_EL1
    uint64_t pc = 0;
    // PSTATE
    uint32_t nzcv = 0;        // bits 31:28
    uint32_t daif = 0x3c0;    // bits 9:6 (D A I F), as in SPSR
    unsigned el = 1;
    bool spsel = true;        // SP_ELx at EL1
    // SIMD and floating point
    u128 v[32] = {};
    uint32_t fpcr = 0, fpsr = 0;
    // EL1 system registers
    uint64_t sctlr = 0x30d00800, actlr = 0, cpacr = 0, ttbr0 = 0, ttbr1 = 0, tcr = 0, mair = 0, amair = 0,
             vbar = 0, elr = 0, spsr = 0, esr = 0, far = 0, par = 0, tpidr1 = 0, tpidr0 = 0, tpidrro0 = 0,
             contextidr = 0, afsr0 = 0, afsr1 = 0, mdscr = 0, cntkctl = 0, csselr = 0, oslsr = 0, mdccint = 0;
    uint64_t mpidr = 0;
    // generic timer, EL1 physical and virtual
    uint64_t cntpCval = 0, cntvCval = 0, cntvoff = 0;
    uint32_t cntpCtl = 0, cntvCtl = 0;
    // GIC CPU interface (ICC_*_EL1)
    uint32_t pmr = 0, bpr1 = 2, igrpen1 = 0, icc_ctlr = 0x400;  // CTLR: IDbits 16, PRIbits 5
    uint32_t sre = 7;
    std::vector<uint32_t> activeStack;  // (priority << 16 | intid) of acknowledged interrupts, oldest first
    // the exclusives monitor
    uint64_t exAddr = ~0ull;
    unsigned exSize = 0;
    uint64_t exVal[2] = {};         // -mt: what the exclusive load read (the store compares it)
    // run state (other processors' threads write the atomic ones with -mt)
    uint64_t cycle = 0, insns = 0;
    uint64_t curPc = 0;             // the instruction being executed (data aborts return to it)
    std::atomic<bool> wfi{false}, off{false};  // off: PSCI CPU_OFF, or not started yet
    std::atomic<bool> irqLine{false};          // the GIC asks for an IRQ
    std::atomic<bool> eventReg{false};         // WFE's event register
    std::atomic<bool> running{false};          // -mt: on its thread in this round
    std::atomic<uint32_t> flushReq{0};         // -mt: another processor's TLBI: empty the TLB
    bool tlbiPending = false;                  // -mt: this one asked others to (DSB waits)
    // decoded instruction pages (a64dec.inc), by physical page
    std::unordered_map<uint64_t, std::unique_ptr<CodePage>> code;
    std::vector<std::unique_ptr<CodePage>> deadCode;  // dropped, freed before the next instruction
    uint64_t codeWrites = 0;
    std::unique_ptr<Prof> prof;     // -profile: the counts (a64prof.hpp)

    // The TLB: virtual page -> physical page, permissions, the host page.
    struct Tlb {
        uint64_t tag = ~0ull;      // VA bits 63:12 (top byte as used), with the regime half
        uint64_t pa = 0;           // physical page
        uint8_t* host = nullptr;   // RAM: where the page is
        uint16_t asid = 0;
        bool global = false;
        uint8_t perm = 0;          // 1 EL1 r, 2 EL1 w, 4 EL1 x, 8 EL0 r, 16 EL0 w, 32 EL0 x
    };
    static constexpr unsigned TLB_N = 1024;
    Tlb tlb[TLB_N];
    // Translations recently used for reads, writes and fetches, keyed by the
    // virtual page and whether the access is EL0's: a hit is RAM that the
    // access may use. Emptied with the TLB and on any change of the
    // translation regime (TTBR, TCR, SCTLR writes, TLBI).
    struct Fast {
        uint64_t key = ~0ull;      // (VA >> 12) << 1 | EL0
        uint8_t* host = nullptr;   // the page in host memory
        uint64_t pa = 0;           // the physical page
        CodePage* code = nullptr;  // fastX: the page decoded (null: not kept)
    };
    static constexpr unsigned FAST_N = 256;
    Fast fastR[FAST_N], fastW[FAST_N], fastX[FAST_N];
    void flushFast() {
        for (auto& e : fastR) e.key = ~0ull;
        for (auto& e : fastW) e.key = ~0ull;
        for (auto& e : fastX) e.key = ~0ull;
    }
    void flushTlb() { for (auto& e : tlb) e.tag = ~0ull; flushFast(); }

    uint64_t& spRef() { return (el && spsel) ? sp[1] : sp[0]; }
    uint64_t pstate() const {  // as SPSR keeps it
        return nzcv | daif | (uint64_t)el << 2 | (spsel ? 1 : 0);
    }
    uint16_t asid() const {
        const uint64_t t = (tcr >> 22 & 1) ? ttbr1 : ttbr0;
        return (uint16_t)(t >> 48);
    }
};

}  // namespace a64
}  // namespace vp
