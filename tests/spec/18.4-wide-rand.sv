// IEEE 1800-2023 18.4: a randc variable wider than 16 bits still gives no
// value twice in a cycle (18.4.2), and a rand vector wider than 64 bits is
// randomized over all its bits (18.4.1).
//! icarus-skip: Icarus 13 has no constraints
module top;
  class C;
    randc bit [19:0] x;
    randc bit [31:0] sm;
    rand bit [99:0] w;
    rand bit [127:0] k;
    rand bit [7:0] b;
    constraint c1 { sm inside {[100:107]}; }
    constraint c2 { b < 10; }
  endclass
  class D;
    rand bit [99:0] w;
    rand bit [3:0] n;
    constraint c { w[99] == 1; n == w[3:0]; }
  endclass
  C c = new;
  D d = new;
  bit seen [int];
  bit [19:0] xs [$];
  int dup, bad, hi, diff;
  bit [99:0] pw;
  initial begin
    // randc bit [31:0] limited to 8 values: every 8 calls give each once.
    for (int cyc = 0; cyc < 4; cyc++) begin
      seen.delete();
      for (int i = 0; i < 8; i++) begin
        if (!c.randomize()) bad++;
        if (seen.exists(c.sm)) dup++;
        seen[c.sm] = 1;
        if (c.sm < 100 || c.sm > 107) bad++;
        if (c.b >= 10) bad++;
        if (c.w[99:64] != 0) hi++;
        if (c.w != pw) diff++;
        pw = c.w;
        xs.push_back(c.x);
      end
    end
    $display("sm: dup=%0d bad=%0d", dup, bad);
    $display("wide: upper bits set %0d, changed %0d", hi > 28, diff > 30);
    // randc bit [19:0]: no value twice in 32 calls.
    dup = 0;
    for (int i = 0; i < 32; i++) for (int j = 0; j < i; j++) if (xs[i] == xs[j]) dup++;
    $display("x: dup=%0d", dup);
    // A constraint that reads a wide variable holds when randomize returns 1.
    bad = 0;
    for (int i = 0; i < 8; i++)
      if (d.randomize()) begin if (!d.w[99] || d.n != d.w[3:0]) bad++; end
      else bad++;
    $display("constrained wide: bad=%0d", bad);
    c.w.rand_mode(0);
    pw = c.w;
    void'(c.randomize());
    $display("rand_mode(0) keeps it: %0d", c.w == pw);
  end
endmodule
//> sm: dup=0 bad=0
//> wide: upper bits set 1, changed 1
//> x: dup=0
//> constrained wide: bad=0
//> rand_mode(0) keeps it: 1
