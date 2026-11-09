// IEEE 1800-2023 23.8: An upward hierarchical reference from a child into
// its parent (23.8).
module m();
  initial #1 $display("%0d", top.v);
endmodule
module top;
  int v = 4;
  m u();
endmodule

//> 4
