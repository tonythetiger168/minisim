// IEEE 1800-2023 20.14: $dist_uniform with a zero-width range returns its
// bound (20.14).
//! xfail: distribution functions are errors
module top;
  int seed = 1;
  initial $display("%0d %0d", $dist_uniform(seed, 4, 4), $dist_uniform(seed, -2, -2));
endmodule

//> 4 -2
