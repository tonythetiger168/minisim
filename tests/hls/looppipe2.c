// Pipelined loops that cannot start an iteration every cycle but can every
// second one: an update in place, a pointer chase, and a loop that ends on
// a value read from memory.
#include <stdint.h>

// Histogram: the bin is read and written by the same iteration, and the
// next iteration may hit the same bin.
void hist(const uint8_t x[64], uint16_t h[16], uint8_t n) {
  for (int i = 0; i < (n & 63); i++) {
#pragma hls pipeline
    uint8_t b = x[i] & 15;
    h[b] = h[b] + 1;
  }
}

// Each iteration follows the pointer the one before read.
uint16_t walk(const uint8_t next[64], const uint8_t w[64], uint8_t start, uint8_t n) {
  uint8_t p = start & 63;
  uint16_t sum = 0;
  for (int i = 0; i < (n & 31); i++) {
#pragma hls pipeline
    p = next[p] & 63;
    sum += w[p];
  }
  return sum;
}

// Copies up to and including the first byte with three low zeros; whether another iteration
// starts is only known when the byte has been read.
uint8_t copyz(const uint8_t s[64], uint16_t d[64], uint8_t key) {
  uint8_t n = 0;
  uint8_t c;
  do {
#pragma hls pipeline
    c = s[(n + key) & 63];
    d[(n + key) & 63] = (uint16_t)(c * key) + n;
    n++;
  } while ((c & 7) != 0 && n < 64);
  return n;
}
