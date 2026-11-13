// IEEE 1800-2023 18.5.9: every combination of legal values is equally
// likely. x < y on 4 bits has 120 solutions, so x = v comes out with
// probability (15 - v) / 120; a and b below have 93 solutions, three of
// them with a > 20 (each 1/93), the rest six per value of a. The counts of
// 24000 calls must sit within 5 standard deviations of those shares.
//! icarus-skip: Icarus has no constrained randomization
class C;
  rand bit [3:0] x, y;
  rand bit [7:0] a, b;
  constraint c1 { x < y; }
  constraint c2 { a % 7 == b % 5; a < 40; b < 30; (a > 20) -> (b == 3); }
endclass
module t;
  C c;
  int cx[16], ca[64];
  int n = 24000, bad = 0;
  function automatic void check(string what, int got, real p);
    real m = n * p, sd = $sqrt(n * p * (1 - p));
    if (got < m - 5 * sd || got > m + 5 * sd) begin
      bad++;
      $display("%s: %0d, expected about %0.0f", what, got, m);
    end
  endfunction
  initial begin
    c = new;
    for (int i = 0; i < n; i++) begin
      void'(c.randomize());
      cx[c.x]++; ca[c.a]++;
    end
    for (int v = 0; v < 16; v++) check($sformatf("x=%0d", v), cx[v], (15 - v) / 120.0);
    for (int v = 0; v < 64; v++) begin
      automatic real p = 0;
      if (v < 21 && v % 7 < 5) p = 6 / 93.0;
      else if (v == 24 || v == 31 || v == 38) p = 1 / 93.0;
      check($sformatf("a=%0d", v), ca[v], p);
    end
    $display("bad %0d", bad);
  end
endmodule
//> bad 0
