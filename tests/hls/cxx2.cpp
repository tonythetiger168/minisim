// More C++ through clang: a nested class, an unnamed enum, static and template
// member functions, operator[], overloads, default arguments, a short initializer
// list, member functions that return *this.
#include <stdint.h>

struct Pixel {
  uint8_t r, g, b;
  uint16_t luma() const { return uint16_t(r * 2 + g * 5 + b); }
};

class Window {
 public:
  enum { kSize = 3 };
  struct Tap { uint16_t v; bool ok; };
  Window() : n_(0) {
    for (int i = 0; i < kSize; i++) { taps_[i].v = 0; taps_[i].ok = false; }
  }
  void push(uint16_t v, bool ok = true) {
    for (int i = kSize - 1; i > 0; i--) taps_[i] = taps_[i - 1];
    taps_[0].v = v;
    taps_[0].ok = ok;
    if (n_ < kSize) n_++;
  }
  uint16_t operator[](int i) const { return taps_[i].ok ? taps_[i].v : 0; }
  bool full() const { return n_ == kSize; }
  static uint16_t mid(uint16_t a, uint16_t b, uint16_t c) {
    uint16_t lo = a < b ? a : b, hi = a < b ? b : a;
    return c < lo ? lo : c > hi ? hi : c;
  }
  uint16_t median() const { return mid((*this)[0], (*this)[1], this->operator[](2)); }
  template <class F> F scaled(F k) const { return F(median() * k); }

 private:
  Tap taps_[kSize];
  uint8_t n_;
};

template <int SH, class T = uint16_t>
T shr(T x) { return T(x >> SH); }

static const uint16_t kGain[4] = {256, 300};

inline uint32_t mix(uint32_t a, uint32_t b) { return a * 31 + b; }
inline uint32_t mix(uint32_t a, uint8_t b, bool flip) { return flip ? mix(b, a) : mix(a, b); }

uint32_t filt(const Pixel px[5], uint8_t sel, Pixel& last) {
  Window w;
  uint32_t h = 0;
  for (int i = 0; i < 5; i++) {
    auto y = px[i].luma();
    if (sel >> i & 1) w.push(y); else w.push(y, false);
    if (w.full()) h = mix(h, shr<2>(w.median()));
    else h = mix(h, uint8_t(w[0]), i == 1);
  }
  last = px[sel % 5];
  last.r = static_cast<uint8_t>(w.scaled<uint32_t>(kGain[sel & 3]) >> 8);
  return h + w.scaled<uint8_t>(3);
}

// Member functions that return *this: compound assignment, increment, chaining.
struct Sat8 {
  uint8_t v;
  Sat8& operator+=(uint8_t x) {
    uint16_t s = uint16_t(v + x);
    v = s > 255 ? uint8_t(255) : uint8_t(s);
    return *this;
  }
  Sat8& operator++() { return *this += 1; }
  Sat8& scale(uint8_t k) {
    if (k == 0) return *this;
    v = uint8_t(v / k);
    return *this;
  }
  bool operator==(const Sat8& o) const { return v == o.v; }
};

uint16_t tally(const uint8_t x[6], uint8_t k) {
  Sat8 bins[2] = {{0}, {0}};
  Sat8 total = {0};
  for (int i = 0; i < 6; i++) {
    bins[x[i] & 1] += x[i];
    total += uint8_t(x[i] >> 2);
    if (bins[0] == bins[1]) ++total;
  }
  total.scale(k & 3).scale(2) += 7;
  return uint16_t(bins[0].v + bins[1].v * 3 + (total += 1).v);
}
