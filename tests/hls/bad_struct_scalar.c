// A struct is not a number.
typedef struct { int a; } one;
int scalar(int v) { one q = {v}; int y = q; return y; }
