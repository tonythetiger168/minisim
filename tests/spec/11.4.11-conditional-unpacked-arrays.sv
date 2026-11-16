// IEEE 1800-2023 11.4.11, 10.3, 7.4.5: the conditional operator on fixed-size
// unpacked arrays, in a continuous assignment to a slice and to a sub-array
// and in a procedural assignment. An x condition keeps the elements both
// arms agree on; the others read as the element type's default (Table 7-1).
//! icarus-skip: Icarus cannot slice unpacked arrays
module t;
  logic s;
  logic [1:0] y[3:0], a[3:0], b[3:0];
  logic [1:0] y3[1:0][2:0], a3[1:0][2:0], b3[1:0][2:0];
  int p[2], pa[2], pb[2];
  assign y[3:0] = s ? a[3:0] : b[3:0];
  assign y3[1:0] = s ? a3[1:0] : b3[1:0];
  initial begin
    foreach (a[i]) begin a[i] = i; b[i] = 3 - i; end
    foreach (a3[i, j]) begin a3[i][j] = i + j; b3[i][j] = 2; end
    pa = '{5, 6}; pb = '{5, 7};
    s = 1; p = s ? pa : pb;
    #1 $display("%p %p %p", y, y3, p);
    s = 0; p = s ? pa : pb;
    #1 $display("%p %p %p", y, y3, p);
    s = 1'bx; p = s ? pa : pb;
    #1 $display("%p %p %p", y, y3, p);
  end
endmodule
//> '{3, 2, 1, 0} '{'{3, 2, 1}, '{2, 1, 0}} '{5, 6}
//> '{0, 1, 2, 3} '{'{2, 2, 2}, '{2, 2, 2}} '{5, 7}
//> '{x, x, x, x} '{'{x, 2, x}, '{2, x, x}} '{5, 0}
