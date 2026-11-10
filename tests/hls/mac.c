// Multiply-accumulate: several multipliers sharing units under -mul 1, static accumulator, in/out pointer.
long long mac(int a, int b, int c, int d, long long *acc, int clear) {
  static long long total = 0;
  long long p = (long long)a * b + (long long)c * d;
  long long q = (long long)(a - c) * (b + d);
  if (clear) total = 0;
  total += p;
  *acc += q;
  return total ^ (q >> 7);
}
