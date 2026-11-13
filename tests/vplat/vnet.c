// vnet.c - a bare-metal virtio-net driver for mvplat's -net (VIRTIO 1.2: 3.1
// initialization, 4.2 MMIO, 2.7 split virtqueues, 5.1 network device). It
// asks for the gateway's MAC address (ARP) and pings it (ICMP echo).
// With -net user the built-in gateway 10.0.2.2 answers; with a peer that
// sends every frame back, the ARP request itself comes back.
#include "rt.h"

#define VIO 0x10001000u
#define R(o) REG32(VIO + (o))

void exit(int code) { exit_(code); }

struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
#define QN 4
#define HDR 12
struct vq {
    struct desc desc[QN];
    volatile uint16_t avail[3 + QN];
    volatile uint32_t used[1 + 2 * QN];
    uint16_t next;
};
static struct vq rxq __attribute__((aligned(16))), txq __attribute__((aligned(16)));
static uint8_t rxbuf[QN][1536], txbuf[1536];
static int fails;
static const uint8_t me[4] = {10, 0, 2, 15}, gw[4] = {10, 0, 2, 2};
static uint8_t mac[6], gwmac[6];

static void check(const char* what, int ok) {
    if (!ok) { puts_("FAIL "); puts_(what); puts_("\n"); fails++; }
}
static uint16_t used_idx(struct vq* q) { return ((volatile uint16_t*)q->used)[1]; }

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

static void send(uint32_t len) {
    for (int i = 0; i < HDR; i++) txbuf[i] = 0;
    txq.desc[0] = (struct desc){(uintptr_t)txbuf, HDR + len, 0, 0};
    offer(&txq, 0);
    R(0x050) = 1;
}
static uint16_t rx_seen;
// The next received frame (after the header), or 0.
static uint8_t* recv(uint32_t* len) {
    if (used_idx(&rxq) == rx_seen) return 0;
    uint32_t id = rxq.used[1 + 2 * (rx_seen % QN)], n = rxq.used[2 + 2 * (rx_seen % QN)];
    rx_seen++;
    *len = n - HDR;
    uint8_t* f = rxbuf[id] + HDR;
    return f;
}
static void give_back(uint8_t* f) {
    int id = (int)((f - HDR - rxbuf[0]) / sizeof rxbuf[0]);
    offer(&rxq, id);
    R(0x050) = 0;
}
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t csum(const uint8_t* p, int n) {
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)p[i] << 8 | p[i + 1];
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

int main(int hart) {
    (void)hart;
    check("magic", R(0x000) == 0x74726976);
    check("a network device", R(0x008) == 1);
    R(0x070) = 0;
    R(0x070) = 1 | 2;
    R(0x014) = 0;
    uint32_t f0 = R(0x010);
    check("MAC offered", f0 & (1u << 5));
    R(0x024) = 1; R(0x020) = 1;            // VERSION_1
    R(0x024) = 0; R(0x020) = 1u << 5;      // MAC
    R(0x070) = 1 | 2 | 8;
    check("FEATURES_OK kept", R(0x070) & 8);
    setup(0, &rxq);
    setup(1, &txq);
    for (int i = 0; i < QN; i++) {
        rxq.desc[i] = (struct desc){(uintptr_t)rxbuf[i], sizeof rxbuf[i], 2, 0};
        offer(&rxq, i);
    }
    R(0x070) = 1 | 2 | 8 | 4;
    R(0x050) = 0;
    for (int i = 0; i < 6; i++) mac[i] = REG8(VIO + 0x100 + i);
    check("the MAC address", mac[0] == 0x52 && mac[1] == 0x54);

    // ARP: who has 10.0.2.2?
    uint8_t* e = txbuf + HDR;
    for (int i = 0; i < 6; i++) { e[i] = 0xff; e[6 + i] = mac[i]; }
    put16(e + 12, 0x0806);
    put16(e + 14, 1); put16(e + 16, 0x0800); e[18] = 6; e[19] = 4; put16(e + 20, 1);
    for (int i = 0; i < 6; i++) { e[22 + i] = mac[i]; e[32 + i] = 0; }
    for (int i = 0; i < 4; i++) { e[28 + i] = me[i]; e[38 + i] = gw[i]; }
    send(42);
    check("the transmit chain came back", used_idx(&txq) == 1);
    uint32_t len = 0;
    uint8_t* f = 0;
    for (int spin = 0; spin < 200000000 && !(f = recv(&len)); spin++) {}
    check("a frame came in", f != 0);
    if (!f) return fails;
    check("an interrupt is pending", R(0x060) & 1);
    R(0x064) = R(0x060);
    puts_("vnet: received ARP op ");
    putd((uint32_t)(f[20] << 8 | f[21]));
    puts_("\n");
    if (f[21] == 1) {  // our own request, sent back by a peer
        check("the echoed frame is ours", len == 42 && f[6] == mac[0] && f[11] == mac[5]);
        if (!fails) puts_("vnet: echo peer ok\n");
        return fails;
    }
    check("an ARP reply from 10.0.2.2", f[21] == 2 && f[28] == 10 && f[31] == 2);
    for (int i = 0; i < 6; i++) gwmac[i] = f[22 + i];
    give_back(f);

    // ICMP echo to 10.0.2.2.
    for (int i = 0; i < 6; i++) { e[i] = gwmac[i]; e[6 + i] = mac[i]; }
    put16(e + 12, 0x0800);
    uint8_t* ip = e + 14;
    for (int i = 0; i < 20; i++) ip[i] = 0;
    ip[0] = 0x45; put16(ip + 2, 20 + 8 + 4); ip[8] = 64; ip[9] = 1;
    for (int i = 0; i < 4; i++) { ip[12 + i] = me[i]; ip[16 + i] = gw[i]; }
    put16(ip + 10, csum(ip, 20));
    uint8_t* ic = ip + 20;
    ic[0] = 8; ic[1] = 0; ic[2] = ic[3] = 0; put16(ic + 4, 0x1234); put16(ic + 6, 1);
    ic[8] = 'm'; ic[9] = 'v'; ic[10] = 'p'; ic[11] = 'l';
    put16(ic + 2, csum(ic, 12));
    send(14 + 32);
    f = 0;
    for (int spin = 0; spin < 200000000 && !(f = recv(&len)); spin++) {}
    check("an echo reply came in", f != 0);
    if (f) {
        uint8_t* rip = f + 14;
        check("the reply is ICMP echo reply from 10.0.2.2", f[12] == 8 && rip[9] == 1 && rip[12] == 10 && rip[15] == 2 && rip[20] == 0);
        check("the reply's IP checksum", csum(rip, 20) == 0);
        check("the reply's ICMP checksum and data", csum(rip + 20, 12) == 0 && rip[28] == 'm' && rip[31] == 'l');
    }
    if (!fails) puts_("vnet: gateway answered ARP and ping\n");
    return fails;
}
