// What a header defines is written only when the design uses it; std::min and
// std::max come from the standard library the same way.
#include <algorithm>
#include "cxxhdr.hpp"
int16_t ring(const int16_t x[7], uint8_t k) {
  lib::Ring<int32_t, 4> r;
  lib::Sat sat = {-2000, 2000};
  int16_t best = -32768;
  for (int i = 0; i < 7; i++) {
    r.put(lib::weigh(x[i], k));
    best = std::max(best, sat.clip(r.sum()));
  }
  return std::min<int16_t>(best, 1500);
}
