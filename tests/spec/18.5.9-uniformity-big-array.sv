// IEEE 1800-2023 18.5.9: the solutions are equally likely also with many
// random variables. 40 elements of 0..7 summing to 80 are symmetric: every
// element has the same distribution, with mean 2. Picking the elements
// one by one in a fixed order leaves the last ones only what the first
// ones did not take (mean near 0); the mean of the first, a middle and the
// last element over 100 calls must be within 0.8 of 2 (about 5 standard
// deviations).
//! icarus-skip: Icarus has no constrained randomization
class C;
  rand bit [2:0] a[40];
  constraint c { a.sum() with (int'(item)) == 80; }
endclass
module t;
  C c;
  int n = 100, bad = 0;
  real s0 = 0, s20 = 0, s39 = 0;
  initial begin
    c = new;
    repeat (n) begin
      if (!c.randomize()) begin $display("randomize failed"); $finish; end
      if (c.a.sum() with (int'(item)) != 80) bad++;
      s0 += c.a[0]; s20 += c.a[20]; s39 += c.a[39];
    end
    s0 /= n; s20 /= n; s39 /= n;
    if (s0 < 1.2 || s0 > 2.8 || s20 < 1.2 || s20 > 2.8 || s39 < 1.2 || s39 > 2.8) begin
      bad++;
      $display("means %0.2f %0.2f %0.2f", s0, s20, s39);
    end
    $display("bad %0d", bad);
  end
endmodule
//> bad 0
