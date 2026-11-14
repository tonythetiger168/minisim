// vcrypto.inc - the vector cryptography extensions, included in class
// Machine by vector.inc: Zvkned (AES), Zvknha/Zvknhb (SHA-256, SHA-512),
// Zvkg (GHASH) on the OP-VE major opcode, and Zvbc (carry-less multiply)
// from the OP-V integer path. Every instruction works on element groups of
// four elements (128 bits at SEW 32, 256 at SEW 64): vl and vstart must be
// multiples of four, and a register group must hold a whole element group.
// Zvkt (data-independent timing) holds trivially in an instruction set
// simulator.

// ---- AES (FIPS-197): the S-boxes from GF(2^8) inverses and the affine map.
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        a = (uint8_t)(a << 1 ^ (a & 0x80 ? 0x1b : 0));
        b >>= 1;
    }
    return p;
}
static const uint8_t* aesSbox(bool inv) {
    static uint8_t s[2][256];
    static const bool init = [] {
        for (int x = 0; x < 256; x++) {
            uint8_t y = 0;
            for (int c = 1; c < 256 && x; c++)
                if (gmul((uint8_t)x, (uint8_t)c) == 1) { y = (uint8_t)c; break; }
            uint8_t r = y;
            for (int k = 1; k < 5; k++) r ^= (uint8_t)(y << k | y >> (8 - k));
            r ^= 0x63;
            s[0][x] = r;
            s[1][r] = (uint8_t)x;
        }
        return true;
    }();
    (void)init;
    return s[inv];
}
// The state as FIPS-197 numbers its bytes: byte i of the element group in
// memory order is in[i], row i % 4, column i / 4.
static void aesSub(uint8_t* s, bool inv) {
    const uint8_t* t = aesSbox(inv);
    for (int i = 0; i < 16; i++) s[i] = t[s[i]];
}
static void aesShift(uint8_t* s, bool inv) {
    uint8_t o[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!inv) o[r + 4 * c] = s[r + 4 * ((c + r) % 4)];
            else o[r + 4 * ((c + r) % 4)] = s[r + 4 * c];
        }
    memcpy(s, o, 16);
}
static void aesMix(uint8_t* s, bool inv) {
    for (int c = 0; c < 4; c++) {
        uint8_t* a = s + 4 * c;
        uint8_t b[4];
        for (int r = 0; r < 4; r++) {
            if (!inv)
                b[r] = (uint8_t)(gmul(a[r], 2) ^ gmul(a[(r + 1) % 4], 3) ^ a[(r + 2) % 4] ^ a[(r + 3) % 4]);
            else
                b[r] = (uint8_t)(gmul(a[r], 14) ^ gmul(a[(r + 1) % 4], 11) ^ gmul(a[(r + 2) % 4], 13) ^
                                 gmul(a[(r + 3) % 4], 9));
        }
        memcpy(a, b, 4);
    }
}
static uint32_t aesSubWord(uint32_t w) {
    const uint8_t* t = aesSbox(false);
    return (uint32_t)t[w & 0xff] | (uint32_t)t[w >> 8 & 0xff] << 8 | (uint32_t)t[w >> 16 & 0xff] << 16 |
           (uint32_t)t[w >> 24] << 24;
}

// ---- SHA-2 (FIPS 180-4), two rounds per instruction.
template <class U> static U rotr(U x, unsigned n) { return x >> n | x << (sizeof(U) * 8 - n); }
template <class U> static void sha2ms(U* d, const U* s2, const U* s1) {
    // d = W[0..3], s2 = {W4, W9, W10, W11}, s1 = W[12..15]; d becomes W[16..19].
    const bool b = sizeof(U) == 8;
    auto sig0 = [&](U x) { return b ? rotr<U>(x, 1) ^ rotr<U>(x, 8) ^ x >> 7 : rotr<U>(x, 7) ^ rotr<U>(x, 18) ^ x >> 3; };
    auto sig1 = [&](U x) { return b ? rotr<U>(x, 19) ^ rotr<U>(x, 61) ^ x >> 6 : rotr<U>(x, 17) ^ rotr<U>(x, 19) ^ x >> 10; };
    U W[20];
    for (int i = 0; i < 4; i++) W[i] = d[i];
    W[4] = s2[0]; W[9] = s2[1]; W[10] = s2[2]; W[11] = s2[3];
    for (int i = 0; i < 4; i++) W[12 + i] = s1[i];
    for (int t = 16; t < 20; t++) W[t] = sig1(W[t - 2]) + W[t - 7] + sig0(W[t - 15]) + W[t - 16];
    for (int i = 0; i < 4; i++) d[i] = W[16 + i];
}
template <class U> static void sha2c(U* d, const U* s2, U w0, U w1) {
    // d = {h, g, d, c} (element 0 first), s2 = {f, e, b, a}; d becomes the new {f, e, b, a}.
    const bool b = sizeof(U) == 8;
    auto S0 = [&](U x) { return b ? rotr<U>(x, 28) ^ rotr<U>(x, 34) ^ rotr<U>(x, 39) : rotr<U>(x, 2) ^ rotr<U>(x, 13) ^ rotr<U>(x, 22); };
    auto S1 = [&](U x) { return b ? rotr<U>(x, 14) ^ rotr<U>(x, 18) ^ rotr<U>(x, 41) : rotr<U>(x, 6) ^ rotr<U>(x, 11) ^ rotr<U>(x, 25); };
    U H = d[0], G = d[1], D = d[2], C = d[3], F = s2[0], E = s2[1], B = s2[2], A = s2[3];
    for (U w : {w0, w1}) {
        U t1 = H + S1(E) + ((E & F) ^ (~E & G)) + w;
        U t2 = S0(A) + ((A & B) ^ (A & C) ^ (B & C));
        H = G; G = F; F = E; E = D + t1; D = C; C = B; B = A; A = t1 + t2;
    }
    d[0] = F; d[1] = E; d[2] = B; d[3] = A;
}

// ---- GHASH: multiply in GF(2^128) with the bits of each byte reflected, as
// GCM numbers them; the values are the element groups as loaded.
struct U128 { uint64_t lo, hi; };
static U128 brev8x(U128 v) {
    auto b = [](uint64_t x) {
        x = (x & 0x5555555555555555ull) << 1 | (x >> 1 & 0x5555555555555555ull);
        x = (x & 0x3333333333333333ull) << 2 | (x >> 2 & 0x3333333333333333ull);
        return (x & 0x0f0f0f0f0f0f0f0full) << 4 | (x >> 4 & 0x0f0f0f0f0f0f0f0full);
    };
    return {b(v.lo), b(v.hi)};
}
static U128 ghashMul(U128 s, U128 h) {
    s = brev8x(s);
    h = brev8x(h);
    U128 z{0, 0};
    for (int bit = 0; bit < 128; bit++) {
        if ((bit < 64 ? s.lo >> bit : s.hi >> (bit - 64)) & 1) { z.lo ^= h.lo; z.hi ^= h.hi; }
        const bool red = h.hi >> 63;
        h.hi = h.hi << 1 | h.lo >> 63;
        h.lo <<= 1;
        if (red) h.lo ^= 0x87;
    }
    return brev8x(z);
}

int vcrypto(Hart& h, uint32_t insn) {
    const unsigned rd = (insn >> 7) & 31, f3 = (insn >> 12) & 7, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31,
                   vm = (insn >> 25) & 1, f6 = insn >> 26;
    const VType& t = h.vt;
    const unsigned sew = t.sew;
    const uint64_t vl = h.vl;
    if (f3 != 2 || !vm) return VILL;
    const bool sha = f6 >= 0x2d && f6 <= 0x2f;
    if (sha ? (sew != 32 && sew != 64) : sew != 32) return VILL;
    if (vl % 4 || h.vstart % 4) return VILL;
    // A register group must hold one element group (EGW = 4 * SEW).
    const unsigned lregs = t.lmul > 0 ? 1u << t.lmul : 1u;
    const uint64_t groupBits = t.lmul >= 0 ? (uint64_t)h.vlen << t.lmul : (uint64_t)h.vlen >> -t.lmul;
    if (groupBits < 4u * sew) return VILL;
    auto aligned = [&](unsigned r) { return r % lregs == 0 && r + lregs <= 32; };
    const uint64_t g0 = h.vstart / 4, gn = vl / 4;
    const unsigned gB = sew / 2;                                        // bytes per element group
    auto grp = [&](unsigned r, uint64_t g) { return h.vr + (size_t)r * (h.vlen / 8) + g * gB; };
    auto done = [&] { h.vstart = 0; h.mstatus |= MSTATUS_VS; return VOK; };

    // AES rounds: .vv takes each group's round key, .vs group 0 of vs2.
    if ((f6 == 0x28 || f6 == 0x29) && (rs1 <= 3 || rs1 == 7)) {
        const bool vs = f6 == 0x29;
        if (rs1 == 7 && !vs) return VILL;                              // only vaesz.vs
        if (!aligned(rd) || (!vs && !aligned(rs2))) return VILL;
        if (vs && rs2 >= rd && rs2 < rd + lregs) return VILL;
        for (uint64_t g = g0; g < gn; g++) {
            uint8_t s[16], k[16];
            memcpy(s, grp(rd, g), 16);
            memcpy(k, vs ? grp(rs2, 0) : grp(rs2, g), 16);
            switch (rs1) {
                case 0:                                                // vaesdm
                    aesShift(s, true); aesSub(s, true);
                    for (int i = 0; i < 16; i++) s[i] ^= k[i];
                    aesMix(s, true);
                    break;
                case 1:                                                // vaesdf
                    aesShift(s, true); aesSub(s, true);
                    for (int i = 0; i < 16; i++) s[i] ^= k[i];
                    break;
                case 2:                                                // vaesem
                    aesSub(s, false); aesShift(s, false); aesMix(s, false);
                    for (int i = 0; i < 16; i++) s[i] ^= k[i];
                    break;
                case 3:                                                // vaesef
                    aesSub(s, false); aesShift(s, false);
                    for (int i = 0; i < 16; i++) s[i] ^= k[i];
                    break;
                default:                                               // vaesz
                    for (int i = 0; i < 16; i++) s[i] ^= k[i];
                    break;
            }
            memcpy(grp(rd, g), s, 16);
        }
        return done();
    }
    // AES key schedule: vaeskf1.vi (AES-128), vaeskf2.vi (AES-256).
    if (f6 == 0x22 || f6 == 0x2a) {
        if (!aligned(rd) || !aligned(rs2)) return VILL;
        static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
        unsigned rnd = rs1 & 15;
        for (uint64_t g = g0; g < gn; g++) {
            uint32_t cur[4], o[4];
            memcpy(cur, grp(rs2, g), 16);
            if (f6 == 0x22) {
                if (rnd == 0 || rnd > 10) rnd ^= 8;
                uint32_t w = aesSubWord(cur[3] >> 8 | cur[3] << 24) ^ rcon[rnd - 1];
                o[0] = w ^ cur[0];
            } else {
                if (rnd < 2 || rnd > 14) rnd ^= 8;
                uint32_t prev[4];
                memcpy(prev, grp(rd, g), 16);
                uint32_t w = rnd & 1 ? aesSubWord(cur[3]) : aesSubWord(cur[3] >> 8 | cur[3] << 24) ^ rcon[rnd / 2 - 1];
                o[0] = w ^ prev[0];
                o[1] = o[0] ^ prev[1]; o[2] = o[1] ^ prev[2]; o[3] = o[2] ^ prev[3];
                memcpy(grp(rd, g), o, 16);
                continue;
            }
            o[1] = o[0] ^ cur[1]; o[2] = o[1] ^ cur[2]; o[3] = o[2] ^ cur[3];
            memcpy(grp(rd, g), o, 16);
        }
        return done();
    }
    // GHASH: vghsh.vv vd = (vd ^ vs1) * vs2; vgmul.vv vd = vd * vs2.
    if (f6 == 0x2c || (f6 == 0x28 && rs1 == 0x11)) {
        const bool gh = f6 == 0x2c;
        if (!aligned(rd) || !aligned(rs2) || (gh && !aligned(rs1))) return VILL;
        for (uint64_t g = g0; g < gn; g++) {
            U128 y, x{0, 0}, hh;
            memcpy(&y, grp(rd, g), 16);
            memcpy(&hh, grp(rs2, g), 16);
            if (gh) memcpy(&x, grp(rs1, g), 16);
            y.lo ^= x.lo; y.hi ^= x.hi;
            U128 r = ghashMul(y, hh);
            memcpy(grp(rd, g), &r, 16);
        }
        return done();
    }
    // SHA-2: vsha2ms (message schedule), vsha2ch / vsha2cl (two rounds with
    // the high or low two words of vs1).
    if (sha) {
        if (!aligned(rd) || !aligned(rs2) || !aligned(rs1)) return VILL;
        if ((rd >= rs2 && rd < rs2 + lregs) || (rd >= rs1 && rd < rs1 + lregs) || (rs2 >= rd && rs2 < rd + lregs) ||
            (rs1 >= rd && rs1 < rd + lregs))
            return VILL;                                               // vd may not overlap a source
        for (uint64_t g = g0; g < gn; g++) {
            if (sew == 32) {
                uint32_t d[4], a[4], b[4];
                memcpy(d, grp(rd, g), 16); memcpy(a, grp(rs2, g), 16); memcpy(b, grp(rs1, g), 16);
                if (f6 == 0x2d) sha2ms<uint32_t>(d, a, b);
                else sha2c<uint32_t>(d, a, f6 == 0x2e ? b[2] : b[0], f6 == 0x2e ? b[3] : b[1]);
                memcpy(grp(rd, g), d, 16);
            } else {
                uint64_t d[4], a[4], b[4];
                memcpy(d, grp(rd, g), 32); memcpy(a, grp(rs2, g), 32); memcpy(b, grp(rs1, g), 32);
                if (f6 == 0x2d) sha2ms<uint64_t>(d, a, b);
                else sha2c<uint64_t>(d, a, f6 == 0x2e ? b[2] : b[0], f6 == 0x2e ? b[3] : b[1]);
                memcpy(grp(rd, g), d, 32);
            }
        }
        return done();
    }
    return VILL;
}
