// virtio.c - a bare-metal virtio-blk driver for mvplat's -disk (VIRTIO 1.2:
// 3.1 device initialization, 4.2 MMIO registers, 2.7 split virtqueues, 5.2
// block device). The disk is made by tests/mvplat.sh: sector 0 starts with
// "MVPLAT-DISK", every other sector s is filled with the byte s.
#include "rt.h"

#define VIO 0x10001000u
#define R(o) REG32(VIO + (o))

void exit(int code) { exit_(code); }

struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct req { uint32_t type, reserved; uint64_t sector; };

#define QN 8
static struct desc desc[QN] __attribute__((aligned(16)));
static volatile uint16_t avail[3 + QN] __attribute__((aligned(2)));
static volatile uint32_t used[1 + 2 * QN] __attribute__((aligned(4)));
static struct req hdr;
static uint8_t buf[1024];
static volatile uint8_t status;
static uint16_t next_avail;
static int fails;

static void check(const char* what, int ok) {
    if (!ok) { puts_("FAIL "); puts_(what); puts_("\n"); fails++; }
}

// One request: header, data (len bytes; the device writes it for IN), status.
static int request(uint32_t type, uint64_t sector, uint32_t len, int dev_writes) {
    hdr.type = type; hdr.reserved = 0; hdr.sector = sector;
    status = 0xff;
    desc[0] = (struct desc){(uintptr_t)&hdr, sizeof hdr, 1, 1};
    desc[1] = (struct desc){(uintptr_t)buf, len, (uint16_t)(1 | (dev_writes ? 2 : 0)), 2};
    desc[2] = (struct desc){(uintptr_t)&status, 1, 2, 0};
    uint16_t want = (uint16_t)(((volatile uint16_t*)used)[1] + 1);
    avail[2 + next_avail % QN] = 0;
    __asm__ volatile("fence" ::: "memory");
    avail[1] = ++next_avail;
    __asm__ volatile("fence" ::: "memory");
    R(0x050) = 0;  // QueueNotify
    check("the used ring moved", ((volatile uint16_t*)used)[1] == want);
    check("the used element names the chain", used[1 + 2 * ((want - 1) % QN)] == 0);
    check("an interrupt is pending", R(0x060) == 1);
    R(0x064) = 1;  // InterruptACK
    check("the interrupt is acknowledged", R(0x060) == 0);
    return status;
}

int main(int hart) {
    (void)hart;
    check("magic", R(0x000) == 0x74726976);
    check("version 2", R(0x004) == 2);
    check("a block device", R(0x008) == 2);
    R(0x070) = 0;          // reset
    R(0x070) = 1 | 2;      // ACKNOWLEDGE, DRIVER
    R(0x014) = 1;
    check("VIRTIO_F_VERSION_1 offered", R(0x010) & 1);
    R(0x024) = 1; R(0x020) = 1;  // accept VERSION_1 only
    R(0x024) = 0; R(0x020) = 0;
    R(0x070) = 1 | 2 | 8;  // FEATURES_OK
    check("FEATURES_OK kept", R(0x070) & 8);
    R(0x030) = 0;
    check("queue 0 is there", R(0x034) >= QN && R(0x044) == 0);
    R(0x038) = QN;
    R(0x080) = (uint32_t)(uintptr_t)desc; R(0x084) = 0;
    R(0x090) = (uint32_t)(uintptr_t)avail; R(0x094) = 0;
    R(0x0a0) = (uint32_t)(uintptr_t)used; R(0x0a4) = 0;
    R(0x044) = 1;
    R(0x070) = 1 | 2 | 8 | 4;  // DRIVER_OK
    uint32_t cap = R(0x100);
    puts_("virtio-blk: ");
    putd(cap);
    puts_(" sectors\n");

    check("read sector 0", request(0, 0, 512, 1) == 0);
    const char* sig = "MVPLAT-DISK";
    for (int i = 0; sig[i]; i++) check("sector 0 signature", buf[i] == (uint8_t)sig[i]);
    check("read sectors 3 and 4", request(0, 3, 1024, 1) == 0);
    check("sector 3 bytes", buf[0] == 3 && buf[511] == 3 && buf[512] == 4 && buf[1023] == 4);
    const char* msg = "written by the guest";
    for (int i = 0; i < 512; i++) buf[i] = 0;
    for (int i = 0; msg[i]; i++) buf[i] = (uint8_t)msg[i];
    check("write sector 2", request(1, 2, 512, 0) == 0);
    for (int i = 0; i < 512; i++) buf[i] = 0;
    check("read sector 2 back", request(0, 2, 512, 1) == 0);
    for (int i = 0; msg[i]; i++) check("sector 2 holds the write", buf[i] == (uint8_t)msg[i]);
    check("a read past the end is IOERR", request(0, cap, 512, 1) == 1);
    check("flush", request(4, 0, 0, 0) == 0);
    check("an unknown request is UNSUPP", request(99, 0, 0, 0) == 2);
    check("GET_ID", request(8, 0, 20, 1) == 0 && buf[0] == 'm' && buf[1] == 'v');
    if (!fails) puts_("virtio-blk: all checks passed\n");
    return fails;
}
