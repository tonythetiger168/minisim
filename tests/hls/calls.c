// Inlined calls in every position: with loops inside expressions, by
// reference, under ?: and && (which must not run the call when the C
// program would not), 2-D arrays, typedef, enum, #define, do-while.
#include <stdint.h>
#define N 4
typedef uint16_t word;
enum { ADD, SUB, XOR3 = 5 };
static const uint8_t sbox[2][N] = {{3, 1, 4, 1}, {5, 9, 2, 6}};

static unsigned popcount(unsigned v) {
  unsigned n = 0;
  while (v) { n += v & 1; v >>= 1; }
  return n;
}
static void swap(word *a, word *b) { word t = *a; *a = *b; *b = t; }
static int absdiff(int a, int b) { if (a > b) return a - b; return b - a; }
static unsigned counter(void) { static unsigned calls = 0; return ++calls; }
static void accumulate(unsigned &sum, const unsigned v[N]) {
  for (int i = 0; i < N; i++) sum += v[i];
}

unsigned calls(word a, word b, uint8_t op, unsigned &total, word *hi) {
  unsigned t[N];
  unsigned r = popcount(a) * 3 + popcount(b ^ a);        // two calls with loops in one expression
  if (a < b) swap(&a, &b);
  *hi = a;
  for (int i = 0; i < N; i++) t[i] = sbox[i & 1][(a >> i) & 3] + (unsigned)absdiff(a, b + i);
  accumulate(total, t);
  unsigned sel = op == ADD ? a + b : op == SUB ? a - b : op == XOR3 ? popcount(a ^ b) : 7u;
  if (op > 8 && popcount(op) == 2) r += 1000;            // right side has a loop
  int k = 0;
  do { r ^= t[k] << k; k++; } while (k < N && (r & 1));
  return r + sel + counter() + (a != 0 || popcount(b) > 3);
}
