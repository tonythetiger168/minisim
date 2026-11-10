// 4x4 matrix times vector, loops fully unrolled by -unroll-all: 16 multipliers when
// unconstrained, shared down under -mul.
void matmul(const short m[16], short x0, short x1, short x2, short x3, int *y0, int *y1, int *y2, int *y3) {
  short x[4] = {x0, x1, x2, x3};
  int y[4];
  for (int r = 0; r < 4; r++) {
    int acc = 0;
    for (int c = 0; c < 4; c++) acc += m[r * 4 + c] * x[c];
    y[r] = acc;
  }
  *y0 = y[0]; *y1 = y[1]; *y2 = y[2]; *y3 = y[3];
}
