// IEEE 1800-2023 10.3.1 and 6.7: a net declaration assignment on an
// unpacked net array is a continuous assignment to each element, and it
// keeps following the right-hand side.
//! icarus-skip: Icarus 13 cannot parse a net array declaration assignment
module top;
  logic [3:0] a = 4'd5, b = 4'd9;
  wire [2:0] w [1:0] = '{3'd1, 3'd2};
  wire [3:0] f [0:2] = '{a, b, a + b};
  initial begin
    #1 $display("%0d %0d | %0d %0d %0d", w[0], w[1], f[0], f[1], f[2]);
    a = 4'd1;
    #1 $display("%0d %0d %0d", f[0], f[1], f[2]);
  end
endmodule
//> 2 1 | 5 9 14
//> 1 9 10
