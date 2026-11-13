// vec_linux.c - the kernels of vkern.c as a Linux program (no C library,
// raw system calls): each one scalar and vectorized, compared. Under Linux
// the first vector instruction traps and the kernel turns the vector unit
// on for the process, and saves its registers at every context switch.
#include <stdint.h>
static long sys(long n, long a, long b, long c) {
    register long a0 asm("a0") = a, a1 asm("a1") = b, a2 asm("a2") = c, a7 asm("a7") = n;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
static void puts1(const char* s) { long n = 0; while (s[n]) n++; sys(64, 1, (long)s, n); }
void vk_init_s(void), vk_init_v(void);
extern uint64_t (*const vk_all_s[])(void);
extern uint64_t (*const vk_all_v[])(void);
void _start(void) {
    vk_init_s();
    vk_init_v();
    int fails = 0, n = 0;
    for (int round = 0; round < 3; round++)  // again, after other processes may have run
        for (int k = 0; vk_all_s[k]; k++, n++)
            if (vk_all_s[k]() != vk_all_v[k]()) fails++;
    char msg[] = "vec: 00 of 00 kernel runs agree under Linux\n";
    msg[5] = (char)('0' + (n - fails) / 10); msg[6] = (char)('0' + (n - fails) % 10);
    msg[11] = (char)('0' + n / 10); msg[12] = (char)('0' + n % 10);
    puts1(msg);
    sys(93, fails, 0, 0);
    for (;;) {}
}
