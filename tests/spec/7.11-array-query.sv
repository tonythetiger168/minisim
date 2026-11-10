// IEEE 1800-2023 7.11: Array query functions (7.11, 20.7).
module top;
  int a[3:0];
  logic [7:0] m[1:4][2];
  initial begin
    $display("%0d %0d %0d %0d %0d %0d", $size(a), $high(a), $low(a), $left(a), $right(a), $increment(a));
    $display("%0d %0d %0d", $dimensions(m), $unpacked_dimensions(m), $size(m, 2));
    $display("%0d %0d", $left(m, 3), $increment(m, 1));
  end
endmodule

//> 4 3 0 3 0 1
//> 3 2 2
//> 7 -1
