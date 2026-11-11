// Refused: a lambda.
int top(int a) {
  auto twice = [&](int x) { return x + a; };
  return twice(2);
}
