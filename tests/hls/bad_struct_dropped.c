// A returned struct has to go somewhere.
typedef struct { int a; } one;
static one make(int v) { one r; r.a = v; return r; }
int dropped(int v) { make(v); return v; }
