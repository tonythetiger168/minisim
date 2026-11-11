// Integer square root with a large local array (a RAM) used as a lookup that is filled first.
#include <stdint.h>
uint16_t isqrt(uint32_t v) {
  uint32_t r = 0, bit = 1u << 30;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
    else r >>= 1;
    bit >>= 2;
  }
  return (uint16_t)r;
}
uint32_t hist(uint8_t d[128], uint8_t key) {
  uint32_t count[256];
  for (int i = 0; i < 256; i++) count[i] = 0;
  for (int i = 0; i < 128; i++) count[d[i] ^ key]++;
  uint32_t best = 0;
  for (int i = 0; i < 256; i++) if (count[i] >= count[best]) best = i;
  return (best << 16) | isqrt(count[best] * 1000u);
}
