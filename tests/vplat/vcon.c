// vcon.c - a bare-metal virtio-console driver for mvplat's -vcon (VIRTIO
// 1.2: 4.2 MMIO, 2.7 split virtqueues, 5.3 console device): it prints a
// line through the transmit queue, then waits for a line on the receive
// queue (what -input types once the console has been quiet) and echoes it.
#include "rt.h"

#define VIO 0x10001000u
#define R(o) REG32(VIO + (o))

void exit(int code) { exit_(code); }

struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
#define QN 4
struct vq {
    struct desc desc[QN];
    volatile uint16_t avail[3 + QN];
    volatile uint32_t used[1 + 2 * QN];
    uint16_t next;
};
static struct vq rxq __attribute__((aligned(16))), txq __attribute__((aligned(16)));
static uint8_t rxbuf[64];
static char line[80];
static int fails;

static void check(const char* what, int ok) {
    if (!ok) { puts_("FAIL "); puts_(what); puts_("\n"); fails++; }
}
static void setup(int sel, struct vq* q) {
    R(0x030) = sel;
    R(0x038) = QN;
    R(0x080) = (uint32_t)(uintptr_t)q->desc; R(0x084) = 0;
    R(0x090) = (uint32_t)(uintptr_t)q->avail; R(0x094) = 0;
    R(0x0a0) = (uint32_t)(uintptr_t)q->used; R(0x0a4) = 0;
    R(0x044) = 1;
}
static void offer(struct vq* q, int d) {
    q->avail[2 + q->next % QN] = (uint16_t)d;
    __asm__ volatile("fence" ::: "memory");
    q->avail[1] = ++q->next;
    __asm__ volatile("fence" ::: "memory");
}
static void vputs(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    txq.desc[0] = (struct desc){(uintptr_t)s, n, 0, 0};
    offer(&txq, 0);
    R(0x050) = 1;
    check("the transmit chain came back", ((volatile uint16_t*)txq.used)[1] == txq.next);
}

int main(int hart) {
    (void)hart;
    check("a console device", R(0x000) == 0x74726976 && R(0x008) == 3);
    R(0x070) = 0;
    R(0x070) = 1 | 2;
    R(0x024) = 1; R(0x020) = 1;  // VERSION_1
    R(0x024) = 0; R(0x020) = 0;
    R(0x070) = 1 | 2 | 8;
    setup(0, &rxq);
    setup(1, &txq);
    rxq.desc[0] = (struct desc){(uintptr_t)rxbuf, sizeof rxbuf, 2, 0};
    offer(&rxq, 0);
    R(0x070) = 1 | 2 | 8 | 4;
    R(0x050) = 0;
    vputs("vcon: hello over virtio-console\n");
    // Read a line, one receive buffer at a time.
    int n = 0;
    uint16_t seen = 0;
    for (long spin = 0; spin < 400000000 && (n == 0 || line[n - 1] != '\n'); spin++) {
        if (((volatile uint16_t*)rxq.used)[1] == seen) continue;
        uint32_t len = rxq.used[2 + 2 * (seen % QN)];
        seen++;
        for (uint32_t i = 0; i < len && n < (int)sizeof line - 1; i++) line[n++] = (char)rxbuf[i];
        R(0x064) = R(0x060);
        offer(&rxq, 0);
        R(0x050) = 0;
    }
    line[n] = 0;
    check("a line came in", n > 0 && line[n - 1] == '\n');
    vputs("vcon: got ");
    vputs(line);
    return fails;
}
