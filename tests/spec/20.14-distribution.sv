// IEEE 1800-2023 20.14: $random and $dist_uniform follow the Annex N
// algorithm: the same seed gives the same values in every simulator (20.14,
// Annex N).
module top;
  int seed = 1;
  integer s = 5;
  initial begin
    $display("%0d %0d", $dist_uniform(seed, 4, 4), $dist_uniform(seed, -2, -2));
    $display("%0d %0d", $random, $random);
    $display("%0d %0d", $random(s), s);
    $display("%0d %0d", $dist_uniform(s, -10, 10), s);
  end
endmodule

//> 4 -2
//> 303379748 -1064739199
//> -2147138048 345346
//> 1 -1917100901
