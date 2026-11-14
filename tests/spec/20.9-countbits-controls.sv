// IEEE 1800-2023 20.9: the control bits of $countbits are any constant
// 1-bit values: '1, '0, 'x, 'z, 1'b1 or a parameter.
module t;
  parameter bit ONE = 1;
  logic [31:0] v = 32'h7000_8421;
  logic [7:0] u = 8'b10xz_01zx;
  initial begin
    $display("%0d %0d %0d %0d", $countbits(v, '1), $countbits(v, '0), $countbits(v, '0, '1), $countbits(v, ONE));
    $display("%0d %0d %0d", $countbits(u, 'x), $countbits(u, 'z), $countbits(u, 'x, 'z, 1'b1));
  end
endmodule
//> 7 25 32 7
//> 2 2 6
