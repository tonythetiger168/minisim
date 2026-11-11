// A small library as a header would hold it: member functions defined outside
// their class template, an inline function declared first, a table.
#pragma once
#include <stdint.h>
namespace lib {
template <class T, int N>
class Ring {
 public:
  Ring();
  void put(T v);
  T sum() const;
 private:
  T buf_[N];
  uint8_t at_;
};
template <class T, int N> Ring<T, N>::Ring() : at_(0) { for (int i = 0; i < N; i++) buf_[i] = 0; }
template <class T, int N> void Ring<T, N>::put(T v) { buf_[at_] = v; at_ = uint8_t(at_ + 1 == N ? 0 : at_ + 1); }
template <class T, int N> T Ring<T, N>::sum() const { T s = 0; for (int i = 0; i < N; i++) s += buf_[i]; return s; }
struct Sat {
  int16_t lo, hi;
  int16_t clip(int32_t v) const;
};
inline int16_t Sat::clip(int32_t v) const { return int16_t(v < lo ? lo : v > hi ? hi : v); }
static const int16_t kBias[3] = {5, -3, 9};
int32_t unusedDecl(int32_t);
int32_t weigh(int32_t v, uint8_t k);
inline int32_t weigh(int32_t v, uint8_t k) { return v * (k + 1) + kBias[k % 3]; }
}
