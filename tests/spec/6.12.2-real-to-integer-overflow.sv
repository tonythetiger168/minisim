// IEEE 1800-2023 6.12.2 and 6.24.1: a real converted to an integral type is
// rounded to the nearest integer and then truncated to the target's size,
// so a value too large for the target keeps its low bits (two's
// complement for a negative value).
module top;
  logic [31:0] v32;
  logic [63:0] v64;
  logic [95:0] v96;
  initial begin
    v32 = 2.0 ** 40 + 3.0;
    v64 = 2.0 ** 70 + 2.0 ** 20;
    $display("%h %h", v32, v64);
    v32 = -(2.0 ** 40) - 3.0;
    v64 = -(2.0 ** 66) - 2.0 ** 20;
    $display("%h %h", v32, v64);
    v96 = 2.0 ** 70 + 2.0 ** 20;
    $display("%h", v96);
    v32 = int'(2.0 ** 33 + 5.4);
    $display("%h", v32);
  end
endmodule
//> 00000003 0000000000100000
//> fffffffd fffffffffff00000
//> 000000400000000000100000
//> 00000005
