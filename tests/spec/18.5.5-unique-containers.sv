// IEEE 1800-2023 18.5.5: unique over dynamic arrays, queues and associative
// arrays (every element differs from the others and from the scalars in the
// list), under an implication, on m[i] of a 2-D dynamic array inside
// foreach, and a queue of 16 four-bit values (all values used once).
//! icarus-skip: Icarus 13 does not support constraints
class C;
  rand bit [3:0] q[$]; rand bit [3:0] d[]; rand bit [3:0] x; rand bit en;
  rand bit [3:0] m[][]; rand byte a[int]; rand bit [3:0] p[$];
  constraint c { m.size() == 3; foreach (m[i]) { m[i].size() == 5; unique {m[i]}; }
                 foreach (a[k]) a[k] inside {[0:3]}; unique {a};
                 p.size() == 16; unique {p};
                 q.size() == 6; d.size() == 4; unique {q}; en -> unique {d, x}; }
  function new; a[5] = 0; a[9] = 0; a[11] = 0; endfunction
endclass
module top; C c = new; int bad, ok = 1;
  initial begin
    repeat (20) begin
      ok &= c.randomize() with { en == 1; };
      foreach (c.q[i]) foreach (c.q[j]) if (i < j && c.q[i] == c.q[j]) bad++;
      foreach (c.d[i]) begin
        if (c.d[i] == c.x) bad++;
        foreach (c.d[j]) if (i < j && c.d[i] == c.d[j]) bad++;
      end
      foreach (c.m[i,j]) foreach (c.m[i][k]) if (j < k && c.m[i][j] == c.m[i][k]) bad++;
      foreach (c.a[x]) foreach (c.a[y]) if (x < y && c.a[x] == c.a[y]) bad++;
      foreach (c.p[x]) foreach (c.p[y]) if (x < y && c.p[x] == c.p[y]) bad++;
    end
    $display("%0d %0d", ok, bad);
  end
endmodule

//> 1 0
