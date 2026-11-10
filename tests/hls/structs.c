// Structs: members, nested structs, arrays of structs, structs passed by
// value, by pointer and by reference, structs returned, bit-fields.
#include <stdint.h>

typedef struct {
  int16_t re, im;
} cplx;

struct pixel {
  uint8_t r, g, b;
};

typedef struct {
  struct pixel px;
  uint16_t x, y;
  unsigned valid : 1;
  unsigned kind : 3;
} sample;

typedef struct {
  uint32_t count;
  int32_t hist[4];
  cplx last;
} stats;

static cplx cmul(cplx a, cplx b) {
  cplx r;
  r.re = (int16_t)((a.re * b.re - a.im * b.im) >> 8);
  r.im = (int16_t)((a.re * b.im + a.im * b.re) >> 8);
  return r;
}

static cplx cadd(cplx a, cplx b) {
  cplx r = {(int16_t)(a.re + b.re), (int16_t)(a.im + b.im)};
  return r;
}

// Structs in, struct out.
cplx cmac(cplx acc, cplx a, cplx b) {
  cplx p = cmul(a, b);
  return cadd(acc, p);
}

static uint8_t luma(struct pixel p) { return (uint8_t)((p.r * 77 + p.g * 150 + p.b * 29) >> 8); }

// A struct by pointer, updated in place; nested members and bit-fields.
uint8_t grade(sample *s, uint8_t gain) {
  uint8_t y = luma(s->px);
  if (!s->valid) return 0;
  s->px.r = (uint8_t)((s->px.r * gain) >> 4);
  s->px.g = (uint8_t)((s->px.g * gain) >> 4);
  s->px.b = (uint8_t)((s->px.b * gain) >> 4);
  s->kind = s->kind + 1;
  s->x += 1;
  if (s->x == 640) {
    s->x = 0;
    s->y++;
  }
  return y;
}

// An array of structs and a struct with an array member, by reference.
int32_t track(cplx in[8], stats &st, uint8_t n) {
  cplx sum = {0, 0};
  cplx taps[3] = {{256, 0}, {128, -64}, {-32, 16}};
  for (int i = 0; i < (n & 7); i++) {
    cplx t = cmul(in[i], taps[i % 3]);
    sum = cadd(sum, t);
    st.hist[(in[i].re >> 14) & 3]++;
    in[i] = t;
  }
  st.count += n & 7;
  st.last = sum;
  return sum.re + sum.im;
}

// Structs in a pipeline: one result per clock.
#pragma hls pipeline
cplx rot(cplx a, uint8_t quarter) {
  cplx r = a;
  switch (quarter & 3) {
    case 1: r.re = (int16_t)-a.im; r.im = a.re; break;
    case 2: r.re = (int16_t)-a.re; r.im = (int16_t)-a.im; break;
    case 3: r.re = a.im; r.im = (int16_t)-a.re; break;
    default: break;
  }
  return r;
}
