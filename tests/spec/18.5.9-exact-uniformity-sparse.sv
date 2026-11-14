// IEEE 1800-2023 18.5.9: every combination of legal values is equally
// likely, also when the legal ones are a thin part of a big space. p and q
// below 256 with p * q equal to 360 or 720 have 48 solutions among 65536
// combinations: too few to be hit by drawing points at random, so they are
// listed. Some values of p have two solutions and some one (p = 2: q = 180;
// p = 3: q = 120 or 240), so p = 3 must come out about twice as often as
// p = 2; picking p first and then q would not do that. Over 1200 calls the
// chi-square of the 48 counts (47 degrees of freedom, mean 47) must stay
// under 100; picking p first gives about 150.
//! icarus-skip: Icarus has no constrained randomization
class C;
  rand bit [7:0] p, q;
  constraint c { p * q == 360 || p * q == 720; }
endclass
module t;
  C c;
  int cnt[int];
  int n = 1200, bad = 0;
  real m, chi = 0;
  initial begin
    c = new;
    for (int i = 0; i < n; i++) begin
      if (!c.randomize()) begin $display("randomize failed"); $finish; end
      if (c.p * c.q != 360 && c.p * c.q != 720) bad++;
      cnt[c.p * 256 + c.q]++;
    end
    m = n / 48.0;
    foreach (cnt[k]) chi += (cnt[k] - m) * (cnt[k] - m) / m;
    chi += (48 - cnt.num()) * m;   // solutions never drawn
    $display("solutions seen %0d, bad %0d, chi-square below 100: %0d", cnt.num(), bad, chi < 100);
  end
endmodule
//> solutions seen 48, bad 0, chi-square below 100: 1
