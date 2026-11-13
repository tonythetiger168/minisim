// linux_init.c - /init of the initramfs the Linux boot test uses: no C library,
// raw system calls. It prints, mounts /proc and prints /proc/version, then
// powers the board off (the finisher ends the run with exit code 0).
typedef unsigned long u64;
static long sys(long n, long a, long b, long c, long d) {
    register long a0 asm("a0") = a, a1 asm("a1") = b, a2 asm("a2") = c, a3 asm("a3") = d, a7 asm("a7") = n;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}
static unsigned len(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void puts1(const char* s) { sys(64, 1, (long)s, len(s), 0); }
void _start(void) {
    puts1("mvplat-init: hello from user space\n");
    char buf[256];
    sys(34, -100, (long)"/proc", 0755, 0);                         // mkdirat
    sys(40, (long)"proc", (long)"/proc", (long)"proc", 0);         // mount
    long fd = sys(56, -100, (long)"/proc/version", 0, 0);  // openat
    if (fd >= 0) { long n = sys(63, fd, (long)buf, sizeof buf - 1, 0); if (n > 0) { buf[n] = 0; puts1(buf); } }
    puts1("mvplat-init: powering off\n");
    sys(142, 0xfee1dead, 672274793, 0x4321fedc, 0);  // reboot(POWER_OFF)
    for (;;) {}
}
