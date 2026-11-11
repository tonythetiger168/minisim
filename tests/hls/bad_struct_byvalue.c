// A struct with an array member cannot be passed by value.
struct buf { int n; int data[4]; };
int byvalue(struct buf x) { return x.n; }
