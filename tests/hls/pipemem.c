// Pipelined functions (-pipeline) with memories: a new call every cycle.
#include <stdint.h>
// One read per call: a table lookup.
int32_t lookup(const int32_t t[256], uint8_t i, int32_t x) { return t[i] * 3 + x; }
// One write per call, not always.
void logw(int32_t m[128], uint8_t i, int32_t x) { if (x > 0) m[i & 127] = x ^ (x >> 3); }
// Read and write the same memory in one call (needs -dual-port): the read
// of a call meets the write of the call before it, and gets that value when
// both go to the same word.
int32_t count(int32_t h[64], uint8_t b, int32_t inc) {
  int32_t v = h[b & 63] + inc;
  h[b & 63] = v;
  return v;
}
// A static local memory: a delay line with a variable tap.
int32_t delay(int32_t x, uint8_t a) {
  static int32_t line[128];
  static uint8_t wp;
  int32_t y = line[(uint8_t)(wp - a) & 127];
  line[wp & 127] = x;
  wp++;
  return y;
}
// Two memories, one of them updated.
int32_t xfer(const int16_t src[128], int32_t acc[128], uint8_t i, uint8_t j) {
  int32_t v = acc[j & 127] + src[i & 127];
  acc[j & 127] = v;
  return v - src[i & 127] * 2;
}

// Updates the same few words over and over, so most calls take the value the
// call before them writes.
int32_t count4(int32_t h[64], uint8_t b, int32_t inc) {
  int32_t v = h[b & 3] + inc;
  if (inc & 1) h[b & 3] = v;
  return v;
}
