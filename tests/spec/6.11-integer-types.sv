// IEEE 1800-2017 6.11 Integer data types: int, byte, shortint, longint are
// 2-state signed; integer is 4-state signed; bit is 2-state unsigned.
module t;
  int a; byte b; shortint c; longint d; bit [3:0] f;
  initial begin
    a = -5; b = 8'hff; c = 16'h8000; d = -1; f = 4'b1010;
    $display("%0d %0d %0d %0d %b", a, b, c, d, f);
    $display("%d|%d", a, b);
  end
endmodule
//> -5 -1 -32768 -1 1010
//>          -5|  -1
