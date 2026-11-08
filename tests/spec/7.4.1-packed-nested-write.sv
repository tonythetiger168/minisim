// IEEE 1800-2023 7.4.1 / 11.5.1: a select of a packed-array element as an
// assignment target, with constant and variable indices at each level, and
// an indexed part-select counted in elements (p[i +: 2] is 16 bits).
//! icarus-skip: Icarus 13 rejects p[i +: 2] on a multidimensional packed array ("reference to a net or variable is not allowed in a constant expression")
module t;
  logic [3:0][7:0] p;
  int i, j;
  initial begin
    p = 32'h44332211;
    p[3][7:4] = 4'h9;
    i = 1; j = 0;
    p[i][j +: 4] = 4'hc;
    p[2][j] = 1'b0;
    $display("%h", p);
    i = 1;
    $display("%h", p[i +: 2]);
  end
endmodule
//> 94322c11
//> 322c
