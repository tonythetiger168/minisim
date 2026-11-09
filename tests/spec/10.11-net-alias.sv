// IEEE 1800-2023 10.11: alias makes two nets the same net (10.11).
//! icarus-skip: Icarus 13 does not support alias
module top;
  wire [3:0] a, b;
  alias a = b;
  assign a = 4'h5;
  initial #1 $display("%h %h", a, b);
endmodule

//> 5 5
