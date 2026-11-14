// zvk.c - the vector cryptography extensions against published answers:
// AES-128 and AES-256 (FIPS-197 appendix C) encrypted and decrypted with
// Zvkned, one block (.vs round keys) and four blocks at once (.vv); SHA-256
// and SHA-512 of "abc" (FIPS 180-4) with Zvknha/Zvknhb; GHASH with Zvkg on
// GCM's test case 2 (McGrew and Viega); and Zvbc's carry-less multiply
// against a scalar model. clang 18 has no intrinsics for these, so each
// instruction runs from a small assembly helper on element groups in memory.
#include "rt.h"

void exit(int code) { exit_(code); }

void* memcpy(void* d, const void* s, unsigned long n) {
    uint8_t* o = d;
    const uint8_t* i = s;
    while (n--) *o++ = *i++;
    return d;
}
void* memset(void* d, int c, unsigned long n) {
    uint8_t* o = d;
    while (n--) *o++ = (uint8_t)c;
    return d;
}

static int fails, checks;
static void check(const char* what, int ok) {
    checks++;
    if (!ok) { puts_("FAIL "); puts_(what); puts_("\n"); fails++; }
}
static int same(const void* a, const void* b, unsigned n) {
    const uint8_t *x = a, *y = b;
    for (unsigned i = 0; i < n; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static const uint32_t K256[64] = {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
static const uint64_t K512[80] = {0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull, 0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};
static const uint32_t H256[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
static const uint64_t H512[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};

// d = insn(d, s2[, s1]) on n elements of SEW 32 or 64, at LMUL 4 (a
// 256-bit element group fits with VLEN 128).
#define OP2(NAME, SEW, INSN)                                                              \
    static void NAME(void* d, const void* s2, unsigned n) {                               \
        __asm__ volatile("vsetvli zero, %0, e" #SEW ", m4, ta, ma\n"                      \
                         "vle" #SEW ".v v8, (%1)\n"                                       \
                         "vle" #SEW ".v v16, (%2)\n" INSN " v8, v16\n"                    \
                         "vse" #SEW ".v v8, (%1)"                                         \
                         :: "r"(n), "r"(d), "r"(s2) : "memory", "v8", "v9", "v10", "v11", \
                            "v16", "v17", "v18", "v19");                                  \
    }
#define OP3(NAME, SEW, INSN)                                                                    \
    static void NAME(void* d, const void* s2, const void* s1, unsigned n) {                     \
        __asm__ volatile("vsetvli zero, %0, e" #SEW ", m4, ta, ma\n"                            \
                         "vle" #SEW ".v v8, (%1)\n"                                             \
                         "vle" #SEW ".v v16, (%2)\n"                                            \
                         "vle" #SEW ".v v24, (%3)\n" INSN " v8, v16, v24\n"                     \
                         "vse" #SEW ".v v8, (%1)"                                               \
                         :: "r"(n), "r"(d), "r"(s2), "r"(s1) : "memory", "v8", "v9", "v10",     \
                            "v11", "v16", "v17", "v18", "v19", "v24", "v25", "v26", "v27");      \
    }
OP2(aesz_vs, 32, "vaesz.vs")
OP2(aesem_vs, 32, "vaesem.vs")
OP2(aesef_vs, 32, "vaesef.vs")
OP2(aesdm_vs, 32, "vaesdm.vs")
OP2(aesdf_vs, 32, "vaesdf.vs")
OP2(aesem_vv, 32, "vaesem.vv")
OP2(aesef_vv, 32, "vaesef.vv")
OP3(xor3, 32, "vxor.vv")
OP2(gmul, 32, "vgmul.vv")
OP3(ghsh, 32, "vghsh.vv")
OP3(ms32, 32, "vsha2ms.vv")
OP3(ch32, 32, "vsha2ch.vv")
OP3(cl32, 32, "vsha2cl.vv")
OP3(ms64, 64, "vsha2ms.vv")
OP3(ch64, 64, "vsha2ch.vv")
OP3(cl64, 64, "vsha2cl.vv")
// The key schedule: the round number is an immediate.
#define KF1(R) static void kf1_##R(void* d, const void* s) {                                    \
        __asm__ volatile("vsetivli zero, 4, e32, m1, ta, ma\n vle32.v v1, (%1)\n"              \
                         "vaeskf1.vi v2, v1, " #R "\n vse32.v v2, (%0)"                         \
                         :: "r"(d), "r"(s) : "memory", "v1", "v2"); }
#define KF2(R) static void kf2_##R(void* d, const void* s) {                                    \
        __asm__ volatile("vsetivli zero, 4, e32, m1, ta, ma\n vle32.v v1, (%1)\n vle32.v v2, (%0)\n" \
                         "vaeskf2.vi v2, v1, " #R "\n vse32.v v2, (%0)"                         \
                         :: "r"(d), "r"(s) : "memory", "v1", "v2"); }
KF1(1) KF1(2) KF1(3) KF1(4) KF1(5) KF1(6) KF1(7) KF1(8) KF1(9) KF1(10)
KF2(2) KF2(3) KF2(4) KF2(5) KF2(6) KF2(7) KF2(8) KF2(9) KF2(10) KF2(11) KF2(12) KF2(13) KF2(14)
static void (*const kf1[11])(void*, const void*) = {0, kf1_1, kf1_2, kf1_3, kf1_4, kf1_5, kf1_6, kf1_7, kf1_8, kf1_9, kf1_10};
static void (*const kf2[15])(void*, const void*) = {0, 0, kf2_2, kf2_3, kf2_4, kf2_5, kf2_6, kf2_7, kf2_8, kf2_9,
                                                    kf2_10, kf2_11, kf2_12, kf2_13, kf2_14};

static uint8_t hexv(char c) { return (uint8_t)(c <= '9' ? c - '0' : c - 'a' + 10); }
static void unhex(uint8_t* o, const char* s) { for (; *s; s += 2) *o++ = (uint8_t)(hexv(s[0]) << 4 | hexv(s[1])); }

static uint8_t rk[15][16];
static void encrypt(uint8_t* s, int rounds) {
    aesz_vs(s, rk[0], 4);
    for (int r = 1; r < rounds; r++) aesem_vs(s, rk[r], 4);
    aesef_vs(s, rk[rounds], 4);
}
static void decrypt(uint8_t* s, int rounds) {
    aesz_vs(s, rk[rounds], 4);
    for (int r = rounds - 1; r > 0; r--) aesdm_vs(s, rk[r], 4);
    aesdf_vs(s, rk[0], 4);
}

static void t_aes(void) {
    uint8_t pt[16], ct[16], s[16];
    unhex(pt, "00112233445566778899aabbccddeeff");
    // AES-128
    unhex(rk[0], "000102030405060708090a0b0c0d0e0f");
    for (int r = 1; r <= 10; r++) kf1[r](rk[r], rk[r - 1]);
    unhex(ct, "13111d7fe3944a17f307a78b4d2b30c5");
    check("AES-128 last round key (vaeskf1)", same(rk[10], ct, 16));
    memcpy(s, pt, 16);
    encrypt(s, 10);
    unhex(ct, "69c4e0d86a7b0430d8cdb78070b4c55a");
    check("AES-128 encrypt (vaesz, vaesem, vaesef)", same(s, ct, 16));
    decrypt(s, 10);
    check("AES-128 decrypt (vaesdm, vaesdf)", same(s, pt, 16));
    // Four blocks at once with .vv: each group carries its own copy of the key.
    {
        static uint8_t blk[64], key[64], one[64];
        for (int i = 0; i < 64; i++) blk[i] = (uint8_t)(i * 37 + 5);
        memcpy(one, blk, 64);
        for (int b = 0; b < 4; b++) encrypt(one + 16 * b, 10);
        for (int b = 0; b < 4; b++) memcpy(key + 16 * b, rk[0], 16);
        xor3(blk, key, blk, 16);  // round 0: vd = vs2 ^ vs1 = key ^ block
        for (int r = 1; r <= 10; r++) {
            for (int b = 0; b < 4; b++) memcpy(key + 16 * b, rk[r], 16);
            if (r < 10) aesem_vv(blk, key, 16);
            else aesef_vv(blk, key, 16);
        }
        check("AES-128 four blocks with .vv round keys", same(blk, one, 64));
    }
    // AES-256
    unhex(rk[0], "000102030405060708090a0b0c0d0e0f");
    unhex(rk[1], "101112131415161718191a1b1c1d1e1f");
    for (int r = 2; r <= 14; r++) { memcpy(rk[r], rk[r - 2], 16); kf2[r](rk[r], rk[r - 1]); }
    memcpy(s, pt, 16);
    encrypt(s, 14);
    unhex(ct, "8ea2b7ca516745bfeafc49904b496089");
    check("AES-256 encrypt (vaeskf2)", same(s, ct, 16));
    decrypt(s, 14);
    check("AES-256 decrypt", same(s, pt, 16));
}

static void t_ghash(void) {
    // GCM test case 2: K = 0, P = 0^128, IV = 0^96.
    uint8_t h[16] = {0}, c[16], y[16] = {0}, len[16] = {0}, want[16];
    rk[0][0] = 0;
    for (int i = 0; i < 16; i++) rk[0][i] = 0;
    for (int r = 1; r <= 10; r++) kf1[r](rk[r], rk[r - 1]);
    encrypt(h, 10);
    unhex(want, "66e94bd4ef8a2c3b884cfa59ca342b2e");
    check("GCM H = AES_0(0)", same(h, want, 16));
    unhex(c, "0388dace60b6a392f328c2b971b2fe78");
    len[15] = 0x80;  // len(A) = 0, len(C) = 128 bits, big-endian
    ghsh(y, h, c, 4);
    uint8_t g1[16];
    memcpy(g1, c, 16);
    gmul(g1, h, 4);
    check("vgmul = vghsh with Y = 0", same(g1, y, 16));
    ghsh(y, h, len, 4);
    unhex(want, "f38cbb1ad69223dcc3457ae5b6b0f885");
    check("GHASH of GCM test case 2 (vghsh)", same(y, want, 16));
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static void t_sha256(void) {
    uint8_t m[64] = {'a', 'b', 'c', 0x80};
    m[63] = 24;
    static uint32_t G[16][4];
    for (int i = 0; i < 16; i++) G[i / 4][i % 4] = be32(m + 4 * i);
    for (int q = 4; q < 16; q++) {
        uint32_t s2[4] = {G[q - 3][0], G[q - 2][1], G[q - 2][2], G[q - 2][3]};
        memcpy(G[q], G[q - 4], 16);
        ms32(G[q], s2, G[q - 1], 4);
    }
    uint32_t X[4] = {H256[5], H256[4], H256[1], H256[0]}, Y[4] = {H256[7], H256[6], H256[3], H256[2]};
    for (int q = 0; q < 16; q++) {
        uint32_t kw[4];
        for (int i = 0; i < 4; i++) kw[i] = G[q][i] + K256[4 * q + i];
        cl32(Y, X, kw, 4);   // Y: the new {f, e, b, a}; X now {h, g, d, c}
        ch32(X, Y, kw, 4);
    }
    uint32_t out[8] = {X[3] + H256[0], X[2] + H256[1], Y[3] + H256[2], Y[2] + H256[3],
                       X[1] + H256[4], X[0] + H256[5], Y[1] + H256[6], Y[0] + H256[7]};
    static const uint32_t want[8] = {0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223,
                                     0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad};
    check("SHA-256(\"abc\") (vsha2ms, vsha2ch, vsha2cl)", same(out, want, 32));
}

static void t_sha512(void) {
    uint8_t m[128] = {'a', 'b', 'c', 0x80};
    m[127] = 24;
    static uint64_t G[20][4];
    for (int i = 0; i < 16; i++) G[i / 4][i % 4] = be64(m + 8 * i);
    for (int q = 4; q < 20; q++) {
        uint64_t s2[4] = {G[q - 3][0], G[q - 2][1], G[q - 2][2], G[q - 2][3]};
        memcpy(G[q], G[q - 4], 32);
        ms64(G[q], s2, G[q - 1], 4);
    }
    uint64_t X[4] = {H512[5], H512[4], H512[1], H512[0]}, Y[4] = {H512[7], H512[6], H512[3], H512[2]};
    for (int q = 0; q < 20; q++) {
        uint64_t kw[4];
        for (int i = 0; i < 4; i++) kw[i] = G[q][i] + K512[4 * q + i];
        cl64(Y, X, kw, 4);
        ch64(X, Y, kw, 4);
    }
    uint64_t out[8] = {X[3] + H512[0], X[2] + H512[1], Y[3] + H512[2], Y[2] + H512[3],
                       X[1] + H512[4], X[0] + H512[5], Y[1] + H512[6], Y[0] + H512[7]};
    static const uint64_t want[8] = {0xddaf35a193617abaull, 0xcc417349ae204131ull, 0x12e6fa4e89a97ea2ull,
                                     0x0a9eeee64b55d39aull, 0x2192992a274fc1a8ull, 0x36ba3c23a3feebbdull,
                                     0x454d4423643ce80eull, 0x2a9ac94fa54ca49full};
    check("SHA-512(\"abc\") (SEW 64)", same(out, want, 64));
}

static void t_clmul(void) {
    static uint64_t a[8], b[8], lo[8], hi[8];
    uint64_t sd = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 8; i++) { sd = sd * 6364136223846793005ull + 1442695040888963407ull; a[i] = sd; sd = sd * 6364136223846793005ull + 1; b[i] = sd; }
    __asm__ volatile("vsetivli zero, 8, e64, m4, ta, ma\n vle64.v v8, (%0)\n vle64.v v16, (%1)\n"
                     "vclmul.vv v24, v8, v16\n vse64.v v24, (%2)\n vclmulh.vv v24, v8, v16\n vse64.v v24, (%3)"
                     :: "r"(a), "r"(b), "r"(lo), "r"(hi) : "memory", "v8", "v16", "v24");
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        uint64_t l = 0, h = 0;
        for (int k = 0; k < 64; k++)  // over the bits of a this time
            if (a[i] >> k & 1) { l ^= b[i] << k; if (k) h ^= b[i] >> (64 - k); }
        ok &= l == lo[i] && h == hi[i];
    }
    check("vclmul.vv / vclmulh.vv", ok);
    uintptr_t x = 0x87;
    __asm__ volatile("vsetivli zero, 8, e64, m4, ta, ma\n vle64.v v8, (%0)\n vclmul.vx v24, v8, %2\n vse64.v v24, (%1)"
                     :: "r"(a), "r"(lo), "r"(x) : "memory", "v8", "v24");
    ok = 1;
    for (int i = 0; i < 8; i++) ok &= lo[i] == (a[i] ^ a[i] << 1 ^ a[i] << 2 ^ a[i] << 7);
    check("vclmul.vx by 0x87", ok);
}

int main(int hart) {
    if (hart) for (;;) {}
    csr_set(mstatus, 3u << 9);  // VS on
    t_aes();
    t_ghash();
    t_sha256();
    t_sha512();
    t_clmul();
    puts_("zvk: ");
    putd((uint32_t)(checks - fails));
    puts_(" of ");
    putd((uint32_t)checks);
    puts_(fails ? " checks passed\n" : " checks passed, all\n");
    return fails;
}
