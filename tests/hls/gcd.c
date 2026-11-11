// Euclid's algorithm: a data-dependent loop, one state per iteration.
unsigned gcd(unsigned a, unsigned b) {
  while (b != 0) {
    unsigned t = a % b;
    a = b;
    b = t;
  }
  return a;
}
