// IEEE 1800-2023 20.9: a $countbits control bit need not be a constant; it
// is a 1-bit logic value read when the call runs, and only its least
// significant bit counts. A value given twice is counted once.
module top;
  logic [7:0] v = 8'b10xz_1100;
  logic c0, c1;
  logic [3:0] wide;
  initial begin
    c0 = 1'b1; c1 = 1'b0;
    $display("%0d %0d", $countbits(v, c0), $countbits(v, c0, c1));
    c0 = 1'bx; c1 = 1'bz;
    $display("%0d %0d %0d", $countbits(v, c0), $countbits(v, c1), $countbits(v, c0, c1, c0));
    wide = 4'b0110;
    $display("%0d", $countbits(v, wide));
  end
endmodule
//> 3 6
//> 1 1 2
//> 3
