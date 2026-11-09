// IEEE 1800-2023 18.5.5 / 18.5.6: Implication (->) and if-else constraints
// apply their body only when the condition holds; the condition can itself
// be random.
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class C;
    rand bit [1:0] mode;
    rand bit [7:0] len;
    rand bit tiny;
    constraint imp { mode == 0 -> len == 0; tiny -> len < 4; }
    constraint ie { if (mode == 1) { len inside {[10:20]}; } else if (mode == 2) len > 200; else len < 100; }
  endclass
  C c;
  int bad, m[4];
  initial begin
    c = new;
    repeat (400) begin
      void'(c.randomize());
      m[c.mode]++;
      if (c.mode == 0 && c.len != 0) bad++;
      if (c.tiny && c.len >= 4) bad++;
      if (c.mode == 1 && !(c.len inside {[10:20]})) bad++;
      if (c.mode == 2 && c.len <= 200) bad++;
      if (c.mode == 3 && c.len >= 100) bad++;
    end
    $display("%0d %0d", bad, m[0] > 0 && m[1] > 0 && m[2] > 0 && m[3] > 0);
    void'(c.randomize() with { tiny == 1; mode != 0; });
    $display("%0d %0d %0d", c.tiny, c.mode, c.len < 4);
  end
endmodule

//> 0 1
//> 1 3 1
