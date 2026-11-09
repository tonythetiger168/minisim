// IEEE 1800-2023 6.10: A name first used on the left of a continuous assign
// is an implicit 1-bit wire (6.10).
module top;
  assign w = 1'b1;
  initial #1 $display("%b", w);
endmodule

//> 1
