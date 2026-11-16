// 21.3.4.3: %t reads a number in the units of $timeformat, rounded to its
// precision, and returns it in the module's time unit rounded to the
// module's precision. A field of only underscores is not a number: the
// conversion fails and the output keeps its value.
`timescale 1ns / 100ps
module top;
  integer rc, x;
  real r;
  time t;
  initial begin
    x = 99;
    rc = $sscanf("_", "%d", x);
    $display("%0d %0d", rc, x);
    rc = $sscanf("1_234_", "%d", x);
    $display("%0d %0d", rc, x);
    rc = $sscanf("8125", "%t", r);
    t = r;
    $display("%0d %f %0d", rc, r, t);
    $timeformat(-3, 2, " ms", 10);
    rc = $sscanf("8.125", "%t", r);
    $display("%0d %f", rc, r);
  end
endmodule
//> 0 99
//> 1 1234
//> 1 812.500000 813
//> 1 8130000.000000
