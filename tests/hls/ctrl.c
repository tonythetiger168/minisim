// Control flow that cannot be if-converted: break, continue, early return, do-while, nested loops.
unsigned ctrl(unsigned x, unsigned char k, unsigned *steps) {
  unsigned n = 0, acc = 0;
  if (k == 0) { *steps = 0; return x; }
  for (unsigned i = 0; i < 40; i++) {
    if ((x & 1) == 0) { x >>= 1; n++; continue; }
    if (x == 1) break;
    x = 3 * x + 1;
    n += 2;
    if (n > 60u + k) break;
  }
  do {
    for (int j = 0; j < (k & 3); j++) acc += x ^ (unsigned)j;
    acc = acc * 5 + 1;
    k >>= 2;
  } while (k);
  *steps = n;
  return acc;
}
