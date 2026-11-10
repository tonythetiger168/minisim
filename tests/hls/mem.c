// Array parameters become memory ports: read, modify, write, with a helper call.
static int clamp(int v, int lo, int hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }
int mem(int a[16], const short b[16], unsigned char n, int *maxv) {
  int sum = 0, m = -2147483647 - 1;
  for (int i = 0; i < 16; i++) {
    int v = a[i] + b[i] * n;
    if (i & 1) v = clamp(v, -100000, 100000);
    a[i] = v;
    sum += v;
    if (v > m) m = v;
  }
  *maxv = m;
  return sum;
}
