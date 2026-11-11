// Refused: inheritance.
struct Base { int x; };
struct Derived : Base {
  int y;
  int sum() const { return x + y; }
};
int top(int a) {
  Derived d;
  d.x = a;
  d.y = 1;
  return d.sum();
}
