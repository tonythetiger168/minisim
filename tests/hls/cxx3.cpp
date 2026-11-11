// Inheritance through clang: a base class is the first member of the derived
// struct. Base constructors, inherited members and member functions, a
// template base, two bases, a member hidden by the derived class, an object
// seen as its base, and a class whose only constructor is the implicit one.
#include <stdint.h>

struct Counter {
  uint8_t n;
  Counter() : n(0) {}
  void tick() { n++; }
  uint8_t count() const { return n; }
};

template <class T>
struct Range {
  T lo, hi;
  Range() : lo(0), hi(0) {}
  void see(T v) {
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  T span() const { return T(hi - lo); }
};

class Stats : public Counter, public Range<int16_t> {
 public:
  Stats() : sum_(0) {}
  void put(int16_t v) {
    tick();
    see(v);
    sum_ += v;
  }
  int32_t sum() const { return sum_; }
  uint8_t count() const { return uint8_t(Counter::count() * 2); }   // hides Counter::count

 protected:
  int32_t sum_;
};

class Clipped : public Stats {
 public:
  explicit Clipped(int16_t limit) : limit_(limit) {}
  void put(int16_t v) { Stats::put(v > limit_ ? limit_ : v); }

 private:
  int16_t limit_;
};

struct Pair {   // no constructor of its own: the implicit one builds both members
  Stats all;
  Counter odd;
};

static int16_t width(const Range<int16_t>& r) { return r.span(); }

int32_t survey(const int16_t x[6], int16_t limit) {
  Clipped c(limit);
  Pair p;
  for (int i = 0; i < 6; i++) {
    c.put(x[i]);
    p.all.put(x[i]);
    if (x[i] & 1) p.odd.tick();
  }
  Counter seen = c;   // the base part of the object
  return c.sum() + width(c) * 3 + c.count() + seen.count() * 7 + p.all.span() + p.odd.count() * 11 + p.all.Counter::count();
}

// A derived struct as an argument: its base members are ports like any others.
struct Point { int16_t x, y; };
struct Pixel : Point {
  uint8_t level;
  int32_t weight() const { return (x + y) * level; }
};

int32_t shade(const Pixel& p, Pixel& out) {
  out = p;
  out.x = int16_t(p.x + 1);
  out.level = uint8_t(p.level >> 1);
  return p.weight() - out.weight();
}
