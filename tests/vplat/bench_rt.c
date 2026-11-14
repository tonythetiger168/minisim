// bench_rt.c - the little runtime the riscv-tests benchmarks need (their
// common/syscalls.c uses GCC nested functions, which clang lacks): HTIF exit
// through tohost, a trap that fails, and the C library calls they make.
#include <stddef.h>
#include <stdint.h>

volatile uint64_t tohost __attribute__((section(".tohost")));
volatile uint64_t fromhost __attribute__((section(".tohost")));

void setStats(int enable) { (void)enable; }
void __attribute__((noreturn)) tohost_exit(uintptr_t code) {
    tohost = (code << 1) | 1;
    for (;;) {}
}
uintptr_t handle_trap(uintptr_t cause, uintptr_t epc, uintptr_t regs[32]) {
    (void)cause; (void)epc; (void)regs;
    tohost_exit(1337);
}
void exit(int code) { tohost_exit((uintptr_t)code); }
void abort(void) { exit(134); }
int printf(const char* fmt, ...) { (void)fmt; return 0; }
void* memcpy(void* d, const void* s, size_t n) {
    char* a = d; const char* b = s;
    while (n--) *a++ = *b++;
    return d;
}
void* memset(void* d, int c, size_t n) {
    char* a = d;
    while (n--) *a++ = (char)c;
    return d;
}
size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
void thread_entry(int cid, int nc) { (void)nc; while (cid != 0) {} }
int main(int, char**);
void _init(int cid, int nc) {
    thread_entry(cid, nc);
    exit(main(0, 0));
}
