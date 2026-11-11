// Two-port memories (mhls -dual-port).
#include <stdint.h>

// Two reads of one memory in the same state.
int32_t pairs(const int32_t a[32]) {
  int32_t s = 0;
  for (int i = 0; i < 16; i++) s += a[i] * a[31 - i];
  return s;
}

// A memory updated in place, one iteration per cycle: the read of an
// iteration meets the write of the one before it, and gets the written word
// when both go to the same address.
void hist(const uint8_t x[64], uint16_t h[16]) {
  for (int i = 0; i < 16; i++) h[i] = 0;
  for (int i = 0; i < 64; i++) {
#pragma hls pipeline
    h[x[i] & 15] += 1 + (x[i] >> 6);
  }
}

// The same with a write that does not always happen.
void bump(const uint8_t x[100], int32_t w[32], int32_t lim) {
  for (int i = 0; i < 100; i++) {
#pragma hls pipeline
    int k = (x[i] * 5) & 31;
    int32_t v = w[k];
    if (v < lim) w[k] = v * 3 + i;
  }
}

// Read one word and write another in every cycle.
void copyfwd(int32_t a[128]) {
  for (int i = 0; i < 100; i++) {
#pragma hls pipeline
    a[i + 20] = a[i] ^ (i << 3);
  }
}

// A local memory: one read, two writes of which one happens.
int32_t runmax(const int16_t x[80]) {
  int16_t best[128];
  for (int i = 0; i < 128; i++) best[i] = -32768;
  for (int i = 0; i < 80; i++) {
#pragma hls pipeline
    int k = x[i] & 127;
    if (x[i] > best[k]) best[k] = x[i];
    else best[k] = best[k] + 1;
  }
  int32_t s = 0;
  for (int i = 0; i < 128; i += 2) s += best[i] - best[i + 1];
  return s;
}

// Two arrays, three accesses to one of them: every third cycle.
void swp(int32_t a[64], int32_t b[64]) {
  for (int i = 0; i < 64; i++) {
#pragma hls pipeline
    int32_t t = a[i];
    a[i] = b[63 - i];
    b[63 - i] = t + a[(i * 7) & 63];
  }
}

// Three accesses in a body that takes two cycles anyway: refused.
int32_t stencil(int32_t seed) {
  int32_t m[200];
  for (int i = 0; i < 200; i++) m[i] = seed * i + 3;
  for (int i = 1; i < 199; i++) {
#pragma hls pipeline
    m[i] = m[i - 1] + m[i + 1];
  }
  int32_t s = 0;
  for (int i = 0; i < 200; i++) s ^= m[i] + i;
  return s;
}
