// Straight-line arithmetic with C integer promotion, signed and unsigned.
#include <stdint.h>
int32_t arith(int8_t a, uint8_t b, int16_t c, uint32_t d, int64_t e) {
  int32_t x = a * b + c;
  uint8_t n = (uint8_t)(a + b);
  int32_t y = (x >> 3) ^ (int32_t)(d << (b & 7));
  int64_t z = e - (int64_t)c * a;
  if (a < c) y += n; else y -= (int32_t)(d >> 5);
  uint32_t q = d / ((b & 15) + 1u);
  int32_t r = c % ((a & 7) + 9);
  return x + y + (int32_t)z + (int32_t)(z >> 33) + (int32_t)q + r + (a > 3 && d != 7 ? 1 : (c <= -2 || b >= 200u));
}
