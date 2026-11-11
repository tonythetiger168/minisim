// Refused: a virtual function called through a class hierarchy.
struct Shape {
  int w;
  virtual int area() const { return w; }
  int twice() const { return 2 * area(); }
};
struct Square : Shape {
  int area() const override { return w * w; }
};
int top(int a) {
  Square s;
  s.w = a;
  return s.twice();
}
