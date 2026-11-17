// IEEE 1800-2023 18.4: a constraint on the size of a rand dynamic array or
// queue of class handles resizes it. randomize() allocates no objects: up
// to the new size the existing objects stay and are randomized, and any
// element added is null and needs no randomization.
//! icarus-skip: Icarus 13 has no constraints
class Item;
  rand int v;
  constraint c { v inside {[1:9]}; }
endclass
class Shrink;
  rand Item a[];
  Item first;
  function new();
    a = new[6];
    foreach (a[i]) a[i] = new;
    first = a[0];
  endfunction
  constraint c { a.size() == 4; foreach (a[i]) a[i].v > i; }
endclass
class Grow;
  rand Item q[$];
  function new(); repeat (2) begin Item it = new; q.push_back(it); end endfunction
  constraint c { q.size() inside {[4:5]}; foreach (q[i]) if (q[i] != null) q[i].v == 7; }
endclass
module top;
  Shrink s = new;
  Grow g = new;
  initial begin
    automatic int good = 1, nulls = 0;
    if (!s.randomize()) good = 0;
    if (s.a.size() != 4 || s.a[0] != s.first) good = 0;
    foreach (s.a[i]) if (s.a[i] == null || s.a[i].v <= i || s.a[i].v > 9) good = 0;
    if (!g.randomize()) good = 0;
    foreach (g.q[i]) if (g.q[i] == null) nulls++; else if (g.q[i].v != 7) good = 0;
    $display("good=%0d size=%0d nulls=%0d", good, g.q.size() >= 4 && g.q.size() <= 5, nulls == g.q.size() - 2);
  end
endmodule
//> good=1 size=1 nulls=1
