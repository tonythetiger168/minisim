// 21.2.1.3: %E and %G print the exponent with an upper-case E.
module top;
  real r = 1234567890.0;
  initial $display("%e %E %g %G", r, r, r, r);
endmodule
//> 1.234568e+09 1.234568E+09 1.23457e+09 1.23457E+09
