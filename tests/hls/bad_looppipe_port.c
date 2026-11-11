// A memory has one port: an update in place cannot start a new iteration
// every cycle.
#include <stdint.h>
void bump(uint8_t a[64], uint8_t n) {
  for (int i = 0; i < (n & 63); i++) {
#pragma hls pipeline
    a[i] = a[i] + 1;
  }
}
