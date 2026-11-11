// A counter and a checksum that nothing reads: their registers, adders
// and the multiplier are removed, the sum stays.
#include <stdint.h>
uint16_t faint(const uint8_t a[16], uint8_t n) {
  uint16_t s = 0;
  uint32_t dbg = 0, cnt = 0;
  for (int i = 0; i < (n & 15); i++) {
    s += a[i];
    cnt++;
    dbg = dbg * 3 + cnt;
  }
  return s;
}
