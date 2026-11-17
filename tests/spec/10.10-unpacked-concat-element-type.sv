// IEEE 1800-2023 10.10 and 7.10: in an unpacked array concatenation each
// item is assigned to an element as by an assignment, so an item of another
// width or type takes the element type: 300 into a byte queue is 44, a
// signed byte widens with its sign, an integer into a real queue is a real.
module top;
  byte a[$] = {1, 300};
  byte d[] = {7, 257};
  bit [3:0] n[$];
  int w[$];
  real r[$] = {1, 2.5};
  initial begin
    n = {5'h1f, 4'h3};
    w = {8'shff, 4'h5};
    $display("%0d %0d %0d %0d %0d %0d", a[0], a[1], d[0], d[1], n[0], n[1]);
    $display("%0d %0d %0.1f %0.1f", w[0], w[1], r[0], r[1]);
  end
endmodule
//> 1 44 7 1 15 3
//> -1 5 1.0 2.5
