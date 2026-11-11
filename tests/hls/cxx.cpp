// C++ through clang (mhls -clang): classes, constructors, operators, templates,
// namespaces and scoped enums.
#include <stdint.h>
#include "mhls_stream.h"

namespace dsp {

enum class Mode : uint8_t { Pass, Scale = 2, Clip };

template <class T, int FRAC>
struct Fixed {
  T v;
  Fixed() : v(0) {}
  explicit Fixed(T raw) : v(raw) {}
  Fixed operator+(const Fixed& o) const { Fixed r(T(v + o.v)); return r; }
  Fixed operator*(const Fixed& o) const {
    Fixed r(T((int64_t(v) * o.v) >> FRAC));
    return r;
  }
  bool operator<(const Fixed& o) const { return v < o.v; }
  T raw() const { return v; }
};

typedef Fixed<int16_t, 8> Q8;

template <int TAPS>
class Fir {
 public:
  static const int kTaps = TAPS;
  Fir() {
    for (int i = 0; i < TAPS; i++) hist_[i] = Q8();
  }
  Q8 step(Q8 x, const int16_t coef[TAPS]) {
    for (int i = TAPS - 1; i > 0; i--) hist_[i] = hist_[i - 1];
    hist_[0] = x;
    Q8 acc;
    for (int i = 0; i < kTaps; i++) {
      Q8 c(coef[i]);
      acc = acc + hist_[i] * c;
    }
    return acc;
  }

 private:
  Q8 hist_[TAPS];
};

template <class T>
T maxOf(T a, T b) { return a < b ? b : a; }

constexpr int kLimit = 1000;

inline int16_t shape(Mode m, int16_t x) {
  switch (m) {
    case Mode::Pass: return x;
    case Mode::Scale: return int16_t(x * 3 / 2);
    case Mode::Clip: return x > kLimit ? int16_t(kLimit) : x < -kLimit ? int16_t(-kLimit) : x;
  }
  return 0;
}

}   // namespace dsp

int16_t fir4(const int16_t x[6], const int16_t coef[4], uint8_t mode) {
  dsp::Fir<4> f;
  dsp::Q8 best;
  for (int i = 0; i < 6; i++) {
    dsp::Q8 in(dsp::shape(dsp::Mode(mode & 3), x[i]));
    dsp::Q8 y = f.step(in, coef);
    best = dsp::maxOf(best, y);
  }
  return best.raw();
}

struct Stats {
  uint32_t sum;
  uint16_t peak;
  uint8_t n;
  void reset() { sum = 0; peak = 0; n = 0; }
  void put(uint16_t x) {
    sum += x;
    if (x > peak) peak = x;
    n++;
  }
};

uint32_t gather(hls::stream<uint16_t>& in, hls::stream<uint32_t>& out, uint8_t count) {
  Stats s;
  s.reset();
  for (uint8_t i = 0; i < (count & 15); i++) {
    uint16_t v = in.read();
    s.put(v);
    if ((s.n & 3) == 0) out.write(s.sum);
  }
  return dsp::maxOf<uint32_t>(s.sum, s.peak);
}
