// IEEE 1800-2023 20.14: $dist_normal, $dist_exponential, $dist_poisson,
// $dist_chi_square, $dist_t and $dist_erlang follow the Annex N algorithms,
// so a seed gives the same values in every simulator, and each call updates
// its seed (20.14, Annex N).
module top;
  integer seed = 1, s2 = 7, s3 = 3, s4 = 11, s5 = 13, s6 = 17;
  initial begin
    repeat (3) $display("%0d %0d %0d %0d %0d %0d", $dist_normal(seed, 100, 10), $dist_exponential(s2, 50),
      $dist_poisson(s3, 4), $dist_chi_square(s4, 3), $dist_t(s5, 4), $dist_erlang(s6, 2, 20));
    $display("%0d %0d", seed, s6);
  end
endmodule

//> 106 455 0 6 0 83
//> 76 13 2 4 -3 18
//> 102 19 7 9 2 11
//> 797919023 -1369077693
