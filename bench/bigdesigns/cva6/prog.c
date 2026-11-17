/* small bare-metal workload: insertion sort + multiply/divide + checksum.
   returns 0 on success, the failing step otherwise. */
static unsigned long a[64];
int main(void) {
  unsigned long x = 12345;
  for (int i = 0; i < 64; i++) { x = x * 6364136223846793005UL + 1442695040888963407UL; a[i] = x >> 33; }
  for (int i = 1; i < 64; i++) { unsigned long k = a[i]; int j = i - 1; while (j >= 0 && a[j] > k) { a[j+1] = a[j]; j--; } a[j+1] = k; }
  for (int i = 1; i < 64; i++) if (a[i-1] > a[i]) return 1;
  unsigned long s = 0; for (int i = 0; i < 64; i++) s += a[i] / 7 + a[i] % 13;
  volatile unsigned long r = s; if (r == 0) return 2;
  unsigned long f0 = 0, f1 = 1; for (int i = 0; i < 50; i++) { unsigned long t = f0 + f1; f0 = f1; f1 = t; }
  if (f0 != 12586269025UL) return 3;
  return 0;
}
