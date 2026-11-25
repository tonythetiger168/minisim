// androiddev.c - the devices an Android guest expects beyond a Linux one:
// goldfish-rtc (Linux's rtc-goldfish) and virtio-rng (VIRTIO 1.2, 5.4). The
// platform is androiddev.json: the RTC at 0x10100000 (PLIC source 5, epoch
// 2026-01-01), virtio-rng at 0x10001000 (source 1, seed 42).
#include "rt.h"

#define RTC 0x10100000u
#define VIO 0x10001000u
#define PLIC 0x0c000000u
#define R(o) REG32(VIO + (o))

void exit(int code) { exit_(code); }

static int fails;
static void check(const char* what, int ok) {
    if (!ok) { puts_("FAIL "); puts_(what); puts_("\n"); fails++; }
}

static uint64_t rtc_time(void) {
    uint32_t lo = REG32(RTC + 0x00);
    uint32_t hi = REG32(RTC + 0x04);
    return (uint64_t)hi << 32 | lo;
}

struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
#define QN 4
static struct desc desc[QN] __attribute__((aligned(16)));
static volatile uint16_t avail[3 + QN] __attribute__((aligned(2)));
static volatile uint32_t used[1 + 2 * QN] __attribute__((aligned(4)));
static uint8_t buf[64];
static uint16_t next_avail;

// One request for n random bytes into buf; the bytes the device wrote.
static uint32_t rng(uint32_t n) {
    for (int i = 0; i < 64; i++) buf[i] = 0;
    desc[0] = (struct desc){(uintptr_t)buf, n, 2, 0};
    avail[2 + next_avail % QN] = 0;
    __asm__ volatile("fence" ::: "memory");
    avail[1] = ++next_avail;
    __asm__ volatile("fence" ::: "memory");
    R(0x050) = 0;
    check("an interrupt is pending", R(0x060) == 1);
    R(0x064) = 1;
    return used[1 + 2 * ((next_avail - 1) % QN) + 1];
}

int main(int hart) {
    (void)hart;
    // goldfish-rtc: the time starts at the epoch and moves with simulated time.
    uint64_t epoch = 1767225600ull * 1000000000ull;
    uint64_t t0 = rtc_time();
    check("the RTC starts at the epoch", t0 >= epoch && t0 < epoch + 1000000000ull);
    for (volatile int i = 0; i < 1000; i++) {}
    uint64_t t1 = rtc_time();
    check("the RTC moves", t1 > t0);
    // Setting the time (high word, then the low word sets the rest).
    REG32(RTC + 0x04) = 0x1;
    REG32(RTC + 0x00) = 0x0;
    uint64_t t2 = rtc_time();
    check("the RTC was set", t2 >= 0x100000000ull && t2 < 0x100000000ull + 1000000000ull);
    // An alarm 100 us ahead: ALARM_STATUS, then the interrupt.
    REG32(PLIC + 0x2000) = 1u << 5;  // hart 0 M-mode enables source 5
    REG32(PLIC + 0x0004 * 5) = 1;    // priority 1
    REG32(RTC + 0x10) = 1;           // IRQ_ENABLED
    uint64_t when = rtc_time() + 100000;
    REG32(RTC + 0x0c) = (uint32_t)(when >> 32);
    REG32(RTC + 0x08) = (uint32_t)when;
    check("the alarm is armed", REG32(RTC + 0x18) == 1);
    check("no interrupt yet", !(REG32(PLIC + 0x1000) & (1u << 5)));
    int spins = 0;
    while (REG32(RTC + 0x18) && ++spins < 10000000) {}
    check("the alarm went off", REG32(RTC + 0x18) == 0);
    check("not before its time", rtc_time() >= when);
    check("the interrupt is pending", REG32(PLIC + 0x1000) & (1u << 5));
    REG32(RTC + 0x1c) = 1;           // CLEAR_INTERRUPT
    check("the interrupt is cleared", !(REG32(PLIC + 0x1000) & (1u << 5)));

    // virtio-rng: device 4, one queue; bytes, the same for the same seed.
    check("magic", R(0x000) == 0x74726976);
    check("an entropy device", R(0x008) == 4);
    R(0x070) = 0;
    R(0x070) = 1 | 2;
    R(0x024) = 1; R(0x020) = 1;
    R(0x024) = 0; R(0x020) = 0;
    R(0x070) = 1 | 2 | 8;
    R(0x030) = 0;
    R(0x038) = QN;
    R(0x080) = (uint32_t)(uintptr_t)desc; R(0x084) = 0;
    R(0x090) = (uint32_t)(uintptr_t)avail; R(0x094) = 0;
    R(0x0a0) = (uint32_t)(uintptr_t)used; R(0x0a4) = 0;
    R(0x044) = 1;
    R(0x070) = 1 | 2 | 8 | 4;
    check("16 bytes", rng(16) == 16);
    uint32_t a = *(uint32_t*)buf, nz = 0;
    for (int i = 0; i < 16; i++) nz += buf[i] != 0;
    check("random bytes", nz >= 12);
    check("odd sizes", rng(5) == 5 && buf[5] == 0);
    rng(16);
    check("the next bytes differ", *(uint32_t*)buf != a);
    puts_("rng ");
    putx(a);
    puts_("\n");
    if (!fails) puts_("android devices: all checks passed\n");
    return fails;
}
