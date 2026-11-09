// IEEE 1800-2023 18.4 with 7.5 / 7.10: rand containers of containers. The
// outer size is solved first, then each inner container's size, then the
// elements (18.5.8.1), so constraints can use m.size(), m[i].size(),
// m[i].sum() and m[i][j] in nested foreach loops. Three levels, associative
// arrays of queues (the keys stay), enum elements, handles in a 2-D dynamic
// array, and inline with constraints on an inner size. When no sizes work
// for the elements, the sizes are picked again; a failed randomize() leaves
// the arrays as they were (18.6.1).
//! icarus-skip: Icarus 13 does not support constraints
typedef enum bit [1:0] {A = 0, B = 1, D = 3} e_t;
class It;
  rand bit [3:0] v;
  constraint c { v > 10; }
endclass
class C;
  rand int m[][];
  rand bit [3:0] t[$][$][$];
  rand byte a[int][$];
  rand e_t e[][];
  rand It h[][];
  constraint cm { m.size() == 2; m[0].size() == 3;
                  foreach (m[i]) { m[i].size() inside {[1:4]}; m[i].sum() == 20; foreach (m[i][j]) m[i][j] inside {[1:10]}; } }
  constraint ct { t.size() == 2;
                  foreach (t[i]) { t[i].size() == 2;
                    foreach (t[i][j]) { t[i][j].size() == i + j + 1; foreach (t[i][j][k]) t[i][j][k] == i * 4 + j * 2 + k; } } }
  constraint ca { foreach (a[k]) { a[k].size() == k; foreach (a[k][j]) a[k][j] == k * 10 + j; } }
  constraint ce { e.size() == 2; foreach (e[i]) e[i].size() == 3; }
  function new;
    a[1] = {}; a[3] = {};
    h = new[2];
    foreach (h[i]) begin h[i] = new[2]; foreach (h[i][j]) h[i][j] = new; end
  endfunction
endclass
module top;
  C c = new;
  int ok = 1, bad, sizes[2];
  initial begin
    repeat (30) begin
      ok &= c.randomize();
      if (c.m.size() != 2 || c.m[0].size() != 3) bad++;
      foreach (c.m[i]) if (c.m[i].sum() != 20) bad++;
      foreach (c.m[i, j]) if (!(c.m[i][j] inside {[1:10]})) bad++;
      if (c.e.size() != 2) bad++;
      foreach (c.e[i, j]) if (!(c.e[i][j] inside {A, B, D})) bad++;
      foreach (c.h[i, j]) if (c.h[i][j].v <= 10) bad++;
    end
    ok &= c.randomize() with { m[1].size() == 4; };
    $display("%0d %0d %0d %0d", ok, bad, c.m[1].size(), c.e[1].size());
    $display("%p", c.t);
    $display("%p", c.a);
    sizes[0] = c.m[0].size(); sizes[1] = c.m[1].size();
    // m[0].size() cannot be 9: nothing changes
    ok = c.randomize() with { m[0].size() == 9; };
    $display("%0d %0d %0d %0d", ok, c.m.size(), c.m[0].size() == sizes[0], c.m[1].size() == sizes[1]);
  end
endmodule

//> 1 0 4 3
//> '{'{'{0}, '{2, 3}}, '{'{4, 5}, '{6, 7, 8}}}
//> '{1:'{10}, 3:'{30, 31, 32}}
//> 0 2 1 1
