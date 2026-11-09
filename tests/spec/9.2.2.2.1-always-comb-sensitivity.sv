// IEEE 1800-2023 9.2.2.2.1: the implicit sensitivity of always_comb leaves
// out variables the block writes (b). Two always_comb blocks that share a
// module-level loop variable must not wake each other forever.
module top;
  integer i;
  logic [7:0] a[0:2], b[0:2], c[0:2];
  logic [7:0] t, y, x;
  always_comb for (i = 0; i < 3; i++) b[i] = a[i] + 1;
  always_comb for (i = 0; i < 3; i++) c[i] = b[i] * 2;
  always_comb begin t = x + 1; y = t * 2; end
  initial begin
    a[0] = 1; a[1] = 2; a[2] = 3; x = 4;
    #1 $display("%0d %0d %0d %0d", c[0], c[1], c[2], y);
    x = 7;
    #1 $display("%0d", y);
  end
endmodule
//> 4 6 8 10
//> 16
