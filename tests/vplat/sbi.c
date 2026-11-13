// sbi.c - a supervisor-mode payload for OpenSBI (mvplat -bios ... -kernel
// sbi64.elf): started on the boot hart with a0 = hart id, a1 = the device
// tree. It prints through the SBI debug console, checks the device tree,
// starts every other hart through HSM, reads the time, then shuts down
// through SRST with the result (0 = pass).
typedef unsigned long ul;
struct sbiret { long error; long value; };
static struct sbiret sbi(long ext, long fid, ul a0, ul a1, ul a2) {
    register ul r0 asm("a0") = a0, r1 asm("a1") = a1, r2 asm("a2") = a2;
    register long r6 asm("a6") = fid, r7 asm("a7") = ext;
    asm volatile("ecall" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r6), "r"(r7) : "memory");
    return (struct sbiret){(long)r0, (long)r1};
}
#define EXT_BASE 0x10
#define EXT_HSM 0x48534d
#define EXT_SRST 0x53525354
#define EXT_DBCN 0x4442434e
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void put(const char* s) { sbi(EXT_DBCN, 0, slen(s), (ul)s, 0); }
static void putn(ul v) {
    char b[24]; int i = 23; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put(b + i);
}
static unsigned be32(const unsigned char* p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

volatile unsigned started;  // a bit per hart that reached secondary()
static ul nharts;

void secondary(ul hart) {
    __atomic_fetch_or(&started, 1u << hart, __ATOMIC_SEQ_CST);
    sbi(EXT_HSM, 1, 0, 0, 0);  // hart_stop
    for (;;) {}
}
extern char _secondary[];

void smain(ul hart, const unsigned char* dtb) {
    int fail = 0;
    put("sbi: hello from S-mode on hart ");
    putn(hart);
    put("\n");
    if (be32(dtb) != 0xd00dfeed) { put("sbi: bad device tree magic\n"); fail = 1; }
    struct sbiret v = sbi(EXT_BASE, 0, 0, 0, 0);
    put("sbi: SBI spec ");
    putn((ul)v.value >> 24); put("."); putn((ul)v.value & 0xffffff); put("\n");
    // Start every other hart through HSM; a hart id with no hart answers
    // SBI_ERR_INVALID_PARAM, which ends the count.
    unsigned want = 1u << hart;
    for (ul h = 0; h < 8; h++) {
        if (h == hart) continue;
        struct sbiret r = sbi(EXT_HSM, 0, h, (ul)_secondary, h);
        if (r.error == -3) break;
        if (r.error) { put("sbi: hart_start failed\n"); fail = 1; break; }
        want |= 1u << h;
    }
    while ((started | (1u << hart)) != want) {}
    nharts = (ul)__builtin_popcount(want);
    put("sbi: harts running: "); putn(nharts); put("\n");
    ul t0, t1;
    asm volatile("rdtime %0" : "=r"(t0));
    for (volatile int i = 0; i < 10000; i++) {}
    asm volatile("rdtime %0" : "=r"(t1));
    if (t1 <= t0) { put("sbi: time does not advance\n"); fail = 1; }
    put(fail ? "sbi: FAIL\n" : "sbi: PASS\n");
    sbi(EXT_SRST, 0, 0, fail ? 1 : 0, 0);  // shutdown; reason 1 = system failure
    for (;;) {}
}
