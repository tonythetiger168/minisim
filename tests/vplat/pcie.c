// pcie.c - the PCIe host (ECAM) and virtio over PCI (VIRTIO 1.2, 4.1), as a
// bare-metal driver would use them. The platform is pcie.json: ECAM at
// 0x30000000, the memory window at 0x40000000, INTx from PLIC source 32;
// five virtio-rng devices at 01.0 to 05.0; 01.0 and 05.0 share INTA's line (33).
#include "rt.h"

#define ECAM 0x30000000u
#define WIN 0x40000000u
#define CFG(dev, r) (ECAM + ((dev) << 15) + (r))

void exit(int code) { exit_(code); }

static int fails;
static void check(const char* what, int ok) {
    if (!ok) { puts_("FAIL "); puts_(what); puts_("\n"); fails++; }
}

struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
#define QN 4
struct vq {
    struct desc desc[QN] __attribute__((aligned(16)));
    volatile uint16_t avail[3 + QN] __attribute__((aligned(2)));
    volatile uint32_t used[1 + 2 * QN] __attribute__((aligned(4)));
    uint16_t next;
};
static struct vq q1, q5;
static uint8_t buf[64];

// Where the virtio structures of the function at `dev` are in its BAR.
struct fn { uint32_t bar, common, isr, devcfg, notify, mult; };

static int pending(unsigned src) { return (REG32(PLIC + 0x1000 + 4 * (src / 32)) >> (src % 32)) & 1; }

static void setup(unsigned dev, uint32_t bar, struct fn* f, struct vq* q) {
    // BAR0 is 64-bit memory, 16 KiB: all ones read back as the size.
    REG32(CFG(dev, 0x10)) = 0xffffffffu;
    REG32(CFG(dev, 0x14)) = 0xffffffffu;
    check("BAR0 sizes as 16 KiB, 64-bit", REG32(CFG(dev, 0x10)) == 0xffffc004u);
    check("BAR1 is BAR0's high half", REG32(CFG(dev, 0x14)) == 0xffffffffu);
    REG32(CFG(dev, 0x10)) = bar;
    REG32(CFG(dev, 0x14)) = 0;
    check("no decode before memory space is on", REG32(bar + 0x14) == 0xffffffffu);
    REG32(CFG(dev, 0x04)) = 0x6;  // memory space, bus master
    f->bar = bar;
    // The vendor capabilities (4.1.4).
    unsigned seen = 0;
    for (uint32_t c = REG8(CFG(dev, 0x34)); c; c = REG8(CFG(dev, c + 1))) {
        if (REG8(CFG(dev, c)) != 0x09) continue;
        uint32_t type = REG8(CFG(dev, c + 3)), off = REG32(CFG(dev, c + 8));
        check("the structures are in BAR0", REG8(CFG(dev, c + 4)) == 0);
        if (type == 1) f->common = bar + off;
        if (type == 2) { f->notify = bar + off; f->mult = REG32(CFG(dev, c + 16)); }
        if (type == 3) f->isr = bar + off;
        if (type == 4) f->devcfg = bar + off;
        seen |= 1u << type;
    }
    check("common, notify, ISR and device capabilities", seen == 0x1e);
    uint32_t cc = f->common;
    REG8(cc + 0x14) = 0;
    check("one queue", *(volatile uint16_t*)(uintptr_t)(cc + 0x12) == 1);
    REG8(cc + 0x14) = 1 | 2;  // ACKNOWLEDGE, DRIVER
    REG32(cc + 0x00) = 1;
    check("VERSION_1 offered", REG32(cc + 0x04) & 1);
    REG32(cc + 0x08) = 1; REG32(cc + 0x0c) = 1;
    REG32(cc + 0x08) = 0; REG32(cc + 0x0c) = 0;
    REG8(cc + 0x14) = 1 | 2 | 8;  // FEATURES_OK
    check("FEATURES_OK stays", REG8(cc + 0x14) & 8);
    *(volatile uint16_t*)(uintptr_t)(cc + 0x16) = 0;
    check("queue size 256 offered", *(volatile uint16_t*)(uintptr_t)(cc + 0x18) == 256);
    *(volatile uint16_t*)(uintptr_t)(cc + 0x18) = QN;
    check("no MSI-X vector", *(volatile uint16_t*)(uintptr_t)(cc + 0x1a) == 0xffff);
    REG32(cc + 0x20) = (uint32_t)(uintptr_t)q->desc; REG32(cc + 0x24) = 0;
    REG32(cc + 0x28) = (uint32_t)(uintptr_t)q->avail; REG32(cc + 0x2c) = 0;
    REG32(cc + 0x30) = (uint32_t)(uintptr_t)q->used; REG32(cc + 0x34) = 0;
    check("the descriptor address reads back", REG32(cc + 0x20) == (uint32_t)(uintptr_t)q->desc);
    *(volatile uint16_t*)(uintptr_t)(cc + 0x1c) = 1;
    check("the queue is enabled", *(volatile uint16_t*)(uintptr_t)(cc + 0x1c) == 1);
    REG8(cc + 0x14) = 1 | 2 | 8 | 4;  // DRIVER_OK
}

// Hand the device a buffer for n bytes; the device fills it at once.
static void post(const struct fn* f, struct vq* q, uint32_t n) {
    for (int i = 0; i < 64; i++) buf[i] = 0;
    q->desc[0] = (struct desc){(uintptr_t)buf, n, 2, 0};
    q->avail[2 + q->next % QN] = 0;
    __asm__ volatile("fence" ::: "memory");
    q->avail[1] = ++q->next;
    __asm__ volatile("fence" ::: "memory");
    uint16_t off = *(volatile uint16_t*)(uintptr_t)(f->common + 0x1e);
    REG32(f->notify + off * f->mult) = 0;
}

int main(int hart) {
    (void)hart;
    check("nothing at 00.0", REG32(CFG(0, 0)) == 0xffffffffu);
    check("virtio-rng at 01.0", REG32(CFG(1, 0)) == 0x10441af4u);
    check("modern only: revision 1", REG8(CFG(1, 0x08)) == 1);
    check("class: other (ff)", (REG32(CFG(1, 0x08)) >> 8) == 0x00ff00u);
    check("subsystem: virtio device 4", REG32(CFG(1, 0x2c)) == 0x00041af4u);
    check("a capabilities list", REG32(CFG(1, 0x04)) & (1u << 20));
    check("INTA", REG8(CFG(1, 0x3d)) == 1);
    check("nothing at 06.0", REG32(CFG(6, 0)) == 0xffffffffu);
    check("the second device at 05.0", REG32(CFG(5, 0)) == 0x10441af4u);
    check("nothing on bus 1", REG32(ECAM + (1u << 20)) == 0xffffffffu);

    struct fn f1, f5;
    setup(1, WIN, &f1, &q1);
    setup(5, WIN + 0x4000, &f5, &q5);

    // Slot 1 and slot 5 both drive INTA's line: PLIC source 32 + 1.
    const unsigned line = 33;
    check("no interrupt yet", !pending(line));
    post(&f1, &q1, 16);
    check("16 bytes", q1.used[2] == 16);
    uint32_t a = *(uint32_t*)buf, nz = 0;
    for (int i = 0; i < 16; i++) nz += buf[i] != 0;
    check("random bytes", nz >= 12);
    check("INTx asserted", pending(line));
    check("the status register shows it", REG32(CFG(1, 0x04)) & (1u << 19));
    // INTx disable (command bit 10) holds the line low.
    REG32(CFG(1, 0x04)) = 0x6 | 0x400;
    check("INTx disabled", !pending(line));
    REG32(CFG(1, 0x04)) = 0x6;
    check("INTx back", pending(line));
    post(&f5, &q5, 8);
    check("the second device answers", q5.used[2] == 8);
    check("ISR says used buffer", REG8(f1.isr) == 1);
    check("reading the ISR cleared it", REG8(f1.isr) == 0);
    check("the line is still up: 05.0 drives it too", pending(line));
    check("05.0's ISR", REG8(f5.isr) == 1);
    check("the line is down", !pending(line));
    // A reset (status 0) takes the queue away.
    REG8(f5.common + 0x14) = 0;
    check("reset: the queue is off", *(volatile uint16_t*)(uintptr_t)(f5.common + 0x1c) == 0);
    puts_("rng ");
    putx(a);
    puts_("\n");
    if (!fails) puts_("pcie: all checks passed\n");
    return fails;
}
