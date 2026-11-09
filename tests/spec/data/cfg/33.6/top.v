// IEEE 1800-2023 33.6: file top.v
module top();
  adder a1();
  adder a2();
endmodule
module m();   // rtl
  initial $display("%m: %l (top.v)");
endmodule
