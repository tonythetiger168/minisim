// gpu.c - a bare-metal virtio-gpu driver (VIRTIO 1.2, 5.7) on gpu.json: the
// display's size, a 2D resource backed by two pieces of memory, a transfer,
// the scanout and a flush; then a partial transfer, and the errors. The
// picture it leaves is checked by tests/mvplat.sh from the PPM file the
// device writes at the end:
//   pixel (x, y) is red 0x80, green 8y, blue 4x; the 4 x 3 rectangle at
//   (10, 5) is white; pixel (0, 0) was changed in memory but never
//   transferred, so it keeps its old colour.
#include "rt.h"

#define VIO 0x10001000u
#define R(o) REG32(VIO + (o))
#define W 64
#define H 32

void exit(int code) { exit_(code); }

static int fails;
static void check(const char* what, int ok) {
    if (!ok) { puts_("FAIL "); puts_(what); puts_("\n"); fails++; }
}

struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
#define QN 4
static struct desc desc[2][QN] __attribute__((aligned(16)));
static volatile uint16_t avail[2][3 + QN] __attribute__((aligned(2)));
static volatile uint32_t used[2][1 + 2 * QN] __attribute__((aligned(4)));
static uint16_t next_avail[2];

struct hdr { uint32_t type, flags; uint64_t fence; uint32_t ctx; uint8_t ring, pad[3]; };
struct rect { uint32_t x, y, w, h; };

static uint32_t cmd[64];
static uint32_t resp[128];
// The framebuffer, in two places: rows 0..15 in fb0, rows 16..31 in fb1.
static uint32_t fb0[W * H / 2] __attribute__((aligned(4096)));
static uint32_t fb1[W * H / 2] __attribute__((aligned(4096)));
static uint32_t* px(int x, int y) { return y < H / 2 ? &fb0[y * W + x] : &fb1[(y - H / 2) * W + x]; }

// Send `n` bytes of cmd on control queue 0; the reply lands in resp.
static uint32_t send(unsigned n, unsigned rn) {
    unsigned q = 0;
    desc[q][0] = (struct desc){(uintptr_t)cmd, n, 1, 1};
    desc[q][1] = (struct desc){(uintptr_t)resp, rn, 2, 0};
    for (unsigned i = 0; i < rn / 4; i++) resp[i] = 0xdeadbeef;
    uint16_t want = (uint16_t)(((volatile uint16_t*)used[q])[1] + 1);
    avail[q][2 + next_avail[q] % QN] = 0;
    __asm__ volatile("fence" ::: "memory");
    avail[q][1] = ++next_avail[q];
    __asm__ volatile("fence" ::: "memory");
    R(0x050) = q;
    check("the used ring moved", ((volatile uint16_t*)used[q])[1] == want);
    R(0x064) = R(0x060);
    return resp[0];
}
static void head(uint32_t type) {
    for (int i = 0; i < 64; i++) cmd[i] = 0;
    cmd[0] = type;
}

static void setupq(unsigned q) {
    R(0x030) = q;
    check("queue size", R(0x034) >= QN);
    R(0x038) = QN;
    R(0x080) = (uint32_t)(uintptr_t)desc[q]; R(0x084) = 0;
    R(0x090) = (uint32_t)(uintptr_t)avail[q]; R(0x094) = 0;
    R(0x0a0) = (uint32_t)(uintptr_t)used[q]; R(0x0a4) = 0;
    R(0x044) = 1;
}

int main(int hart) {
    (void)hart;
    check("a GPU device", R(0x008) == 16);
    check("one scanout", R(0x100 + 8) == 1);
    check("no capability sets (2D)", R(0x100 + 12) == 0);
    R(0x070) = 0;
    R(0x070) = 1 | 2;
    R(0x024) = 1; R(0x020) = 1;
    R(0x024) = 0; R(0x020) = 0;
    R(0x070) = 1 | 2 | 8;
    setupq(0);
    setupq(1);
    R(0x070) = 1 | 2 | 8 | 4;

    // GET_DISPLAY_INFO
    head(0x0100);
    check("display info", send(24, 24 + 16 * 24) == 0x1101);
    check("scanout 0 is 64 x 32", resp[6] == 0 && resp[7] == 0 && resp[8] == W && resp[9] == H);
    check("scanout 0 is enabled", resp[10] == 1);
    check("scanout 1 is not", resp[16] == 0);

    // RESOURCE_CREATE_2D id 1, B8G8R8X8, 64 x 32
    head(0x0101); cmd[6] = 1; cmd[7] = 2; cmd[8] = W; cmd[9] = H;
    check("create", send(40, 24) == 0x1100);
    check("the same id twice", send(40, 24) == 0x1203);
    cmd[6] = 2; cmd[7] = 999;
    check("an unknown format", send(40, 24) == 0x1205);

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) *px(x, y) = 0x800000u | (uint32_t)(8 * y) << 8 | (uint32_t)(4 * x);

    // RESOURCE_ATTACH_BACKING: two entries
    head(0x0106); cmd[6] = 1; cmd[7] = 2;
    cmd[8] = (uint32_t)(uintptr_t)fb0; cmd[9] = 0; cmd[10] = sizeof fb0;
    cmd[12] = (uint32_t)(uintptr_t)fb1; cmd[13] = 0; cmd[14] = sizeof fb1;
    check("attach backing", send(32 + 32, 24) == 0x1100);

    // TRANSFER_TO_HOST_2D, all of it; with a fence
    head(0x0105); cmd[1] = 1; cmd[2] = 0x1234; cmd[3] = 0x5;
    cmd[6] = 0; cmd[7] = 0; cmd[8] = W; cmd[9] = H; cmd[10] = 0; cmd[11] = 0; cmd[12] = 1;
    check("transfer", send(56, 24) == 0x1100);
    check("the reply carries the fence", resp[1] == 1 && resp[2] == 0x1234 && resp[3] == 0x5);

    // SET_SCANOUT 0 -> resource 1; RESOURCE_FLUSH
    head(0x0103); cmd[8] = W; cmd[9] = H; cmd[10] = 0; cmd[11] = 1;
    check("set scanout", send(48, 24) == 0x1100);
    cmd[10] = 1;
    check("there is no scanout 1", send(48, 24) == 0x1202);
    head(0x0104); cmd[8] = W; cmd[9] = H; cmd[10] = 1;
    check("flush", send(48, 24) == 0x1100);
    check("no fence asked, none given", resp[1] == 0);

    // A partial transfer: the 4 x 3 rectangle at (10, 5) only.
    for (int y = 5; y < 8; y++) for (int x = 10; x < 14; x++) *px(x, y) = 0xffffff;
    *px(0, 0) = 0x00ff00;  // never transferred
    head(0x0105);
    cmd[6] = 10; cmd[7] = 5; cmd[8] = 4; cmd[9] = 3; cmd[10] = 5 * W * 4 + 10 * 4; cmd[12] = 1;
    check("partial transfer", send(56, 24) == 0x1100);
    cmd[12] = 7;
    check("transfer to no resource", send(56, 24) == 0x1203);
    cmd[12] = 1; cmd[8] = W;
    check("a rectangle outside the resource", send(56, 24) == 0x1205);

    // The cursor queue takes a command and gives no reply.
    head(0x0300);
    desc[1][0] = (struct desc){(uintptr_t)cmd, 56, 0, 0};
    avail[1][2] = 0;
    __asm__ volatile("fence" ::: "memory");
    avail[1][1] = 1;
    __asm__ volatile("fence" ::: "memory");
    R(0x050) = 1;
    check("the cursor command is used", ((volatile uint16_t*)used[1])[1] == 1 && used[1][2] == 0);

    head(0x0999);
    check("an unknown command", send(24, 24) == 0x1200);
    // A resource made and dropped
    head(0x0101); cmd[6] = 3; cmd[7] = 1; cmd[8] = 8; cmd[9] = 8;
    check("create 3", send(40, 24) == 0x1100);
    head(0x0102); cmd[6] = 3;
    check("unref 3", send(32, 24) == 0x1100);
    check("unref 3 again", send(32, 24) == 0x1203);

    if (!fails) puts_("gpu: all checks passed\n");
    return fails;
}
