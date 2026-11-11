// The loop ends on a value read from memory, which is not known when the
// next iteration would have to start.
#include <stdint.h>
uint8_t length(const uint8_t s[64]) {
  uint8_t n = 0;
  while (s[n & 63] != 0) {
#pragma hls pipeline
    n++;
  }
  return n;
}
