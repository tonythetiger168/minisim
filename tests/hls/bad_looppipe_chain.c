// Each iteration follows a pointer read by the one before.
#include <stdint.h>
uint8_t chase(const uint8_t next[64], uint8_t start, uint8_t n) {
  uint8_t p = start;
  for (int i = 0; i < (n & 15); i++) {
#pragma hls pipeline
    p = next[p & 63];
  }
  return p;
}
