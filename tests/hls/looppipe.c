// Loop pipelining: a new iteration every clock while earlier ones are still
// in later stages.
#include <stdint.h>

// One memory read per iteration: the address goes out in one stage and the
// data is added in the next, so an unpipelined loop needs two cycles each.
uint32_t sum(const uint16_t a[64], uint8_t n) {
  uint32_t s = 0;
  for (int i = 0; i < (n & 63); i++) {
#pragma hls pipeline
    s += a[i];
  }
  return s;
}

// Read one memory, write another, with a multiply in between and a running
// maximum carried from iteration to iteration.
int16_t scale(const int16_t in[32], int16_t out[32], int16_t gain, uint8_t n) {
  int16_t peak = 0;
  for (int i = 0; i <= (n & 31); i++) {
#pragma hls pipeline
    int32_t y = (in[i] * gain) >> 6;
    int16_t z = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    out[i] = z;
    if (z > peak) peak = z;
  }
  return peak;
}

// A table lookup feeding a checksum, inside an outer loop that is not
// pipelined: the pipeline is entered and drained once per outer iteration.
static const uint8_t sbox[16] = {12, 5, 6, 11, 9, 0, 10, 13, 3, 14, 15, 8, 4, 7, 1, 2};
uint32_t mix(const uint8_t msg[64], uint8_t rounds) {
  uint32_t h = 0x811c9dc5u;
  for (int r = 0; r < (rounds & 3) + 1; r++) {
    for (int i = 0; i < 64; i += 4) {
#pragma hls pipeline
      uint8_t b = msg[i + r];
      h = (h ^ sbox[b & 15] ^ (sbox[b >> 4] << 4)) * 16777619u;
    }
    h ^= h >> 15;
  }
  return h;
}
