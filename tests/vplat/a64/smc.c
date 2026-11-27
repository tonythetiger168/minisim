// smc.c - code that changes: instructions written to RAM and run, then
// rewritten and run again (mvplat keeps decoded instructions per page and
// has to notice), code and data on one page, and the load and store forms
// that have a decoded fast path of their own (indexing, pairs, sign
// extension, register offsets).
#include "rt.h"
void secondary(int cpu) { (void)cpu; }
void exit(int code) { exit_(code); }
void trap(int kind, struct frame* f) { (void)f; puts_("unexpected exception "); putd((uint64_t)kind); puts_(" esr "); putx(SYSREG_READ(esr_el1)); puts_("\n"); exit_(1); }

static uint32_t code[1024] __attribute__((aligned(4096)));
typedef uint64_t (*fn_t)(uint64_t);

static void sync_code(void* p, unsigned n) {
    for (unsigned k = 0; k < n; k += 64) __asm__ volatile("dc cvau, %0" :: "r"((char*)p + k) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (unsigned k = 0; k < n; k += 64) __asm__ volatile("ic ivau, %0" :: "r"((char*)p + k) : "memory");
    __asm__ volatile("dsb ish; isb" ::: "memory");
}
// x0 = 2 * v
static void emit(unsigned at, unsigned v) {
    code[at] = 0xd2800000u | (v & 0xffff) << 5;  // movz x0, #v
    code[at + 1] = 0x8b000000u;                  // add x0, x0, x0
    code[at + 2] = 0xd65f03c0u;                  // ret
}

int main(void) {
    fn_t f = (fn_t)(uintptr_t)code;
    // the same address, rewritten 200 times, each version run 50 times
    int bad = 0;
    for (unsigned v = 1; v <= 200; v++) {
        emit(0, v);
        sync_code(code, 16);
        for (int r = 0; r < 50; r++) if (f(0) != 2 * v) bad++;
    }
    check("rewritten code runs as rewritten", bad == 0);
    // one instruction of a long, decoded block changed in the middle
    for (unsigned k = 0; k < 100; k++) code[16 + k] = 0x91000400u;  // add x0, x0, #1
    code[116] = 0xd65f03c0u;                                       // ret
    sync_code(code, 4096);
    fn_t g = (fn_t)(uintptr_t)&code[16];
    uint64_t a = 0;
    for (int r = 0; r < 20; r++) a += g(0);
    code[66] = 0x91002800u;  // add x0, x0, #10
    sync_code(&code[66], 4);
    const uint64_t b = g(0);
    check("a block of decoded code changed in the middle", a == 2000 && b == 109);
    // a loop storing to the page it runs from
    code[200] = 0x10006401u;  // adr x1, code[1000]
    code[201] = 0xd2800000u;  // movz x0, #0
    code[202] = 0xd2807d02u;  // movz x2, #1000
    code[203] = 0x91000400u;  // add x0, x0, #1
    code[204] = 0xf9000020u;  // str x0, [x1]
    code[205] = 0xf1000442u;  // subs x2, x2, #1
    code[206] = 0x54ffffa1u;  // b.ne 203
    code[207] = 0xd65f03c0u;  // ret
    sync_code(&code[200], 32);
    const uint64_t n = ((fn_t)(uintptr_t)&code[200])(0);
    check("code and data on one page", n == 1000 && *(volatile uint64_t*)&code[1000] == 1000);

    // loads and stores
    static uint64_t buf[8] __attribute__((aligned(16)));
    for (int k = 0; k < 8; k++) buf[k] = 0x8070605040302010ull + (uint64_t)k * 0x0101010101010101ull;
    uint64_t p = (uint64_t)(uintptr_t)buf, q, r1, r2, r3, r4;
    __asm__ volatile("ldr %1, [%0, #8]!" : "+r"(p), "=r"(r1));
    check("ldr pre-index", p == (uint64_t)(uintptr_t)&buf[1] && r1 == buf[1]);
    __asm__ volatile("ldr %1, [%0], #16" : "+r"(p), "=r"(r1));
    check("ldr post-index", p == (uint64_t)(uintptr_t)&buf[3] && r1 == buf[1]);
    __asm__ volatile("ldp %1, %2, [%0, #-16]!" : "+r"(p), "=r"(r1), "=r"(r2));
    check("ldp pre-index", p == (uint64_t)(uintptr_t)&buf[1] && r1 == buf[1] && r2 == buf[2]);
    __asm__ volatile("ldrsb %1, [%0, #7]; ldrsh %w2, [%0, #6]; ldrsw %3, [%0, #4]; ldrb %w4, [%0, #7]"
                     : "+r"(p), "=r"(r1), "=r"(r2), "=r"(r3), "=r"(r4));
    check("sign-extending loads", r1 == 0xffffffffffffff81ull && r2 == 0xffff8171ull && r3 == 0xffffffff81716151ull && r4 == 0x81);
    q = 2;
    __asm__ volatile("ldr %1, [%0, %2, lsl #3]; ldr %w3, [%0, %w2, uxtw #2]" : "+r"(p), "=&r"(r1), "+r"(q), "=&r"(r2));
    check("register offsets", r1 == buf[3] && r2 == (uint32_t)(buf[2]));
    q = (uint64_t)-1;
    __asm__ volatile("ldrb %w1, [%0, %w2, sxtw]" : "+r"(p), "=&r"(r1), "+r"(q));
    check("a negative register offset", r1 == (buf[0] >> 56));
    r1 = 0x1111111111111111ull; r2 = 0x2222222222222222ull;
    __asm__ volatile("stp %1, %2, [%0, #16]!" : "+r"(p) : "r"(r1), "r"(r2) : "memory");
    check("stp pre-index", p == (uint64_t)(uintptr_t)&buf[3] && buf[3] == r1 && buf[4] == r2);
    __asm__ volatile("str %w1, [%0], #-8; strh %w2, [%0, #2]; strb %w2, [%0, #1]" : "+r"(p) : "r"(0xdeadbeefu), "r"(0x5aa5u) : "memory");
    check("stores of 4, 2 and 1 bytes", p == (uint64_t)(uintptr_t)&buf[2] && (uint32_t)buf[3] == 0xdeadbeefu &&
          buf[2] == 0x827262525aa5a512ull);
    // the stack pointer as a base, with writeback
    __asm__ volatile("mov %0, sp; stp x29, x30, [sp, #-32]!; mov %1, sp; ldp x29, x30, [sp], #32; mov %2, sp"
                     : "=&r"(r1), "=&r"(r2), "=&r"(r3) :: "memory");
    check("stp / ldp on sp with writeback", r2 == r1 - 32 && r3 == r1);
    if (!fails) puts_("smc: all passed\n");
    return fails;
}
