// IEEE 1800-2023 18.5.8: Global constraints. The objects of rand class
// handles are randomized with their parent, and the parent's constraints
// can name their members. A null rand handle is skipped. Constraint guards
// (18.5.12) keep a constraint on a null handle from applying.
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class Leaf;
    rand bit [7:0] v;
    constraint lim { v < 100; }
  endclass
  class Node;
    rand Leaf l, r;
    rand bit [7:0] total;
    constraint sum { total == l.v + r.v; l.v > r.v; }
    function new(); l = new; r = new; endfunction
  endclass
  class Opt;
    rand Leaf p;
    rand bit [3:0] k;
    constraint g { p != null -> p.v == k; }
  endclass
  Node n;
  Opt o;
  int bad, ok = 1;
  initial begin
    n = new;
    repeat (50) begin
      ok &= n.randomize();
      if (n.total != n.l.v + n.r.v || n.l.v <= n.r.v || n.l.v >= 100 || n.r.v >= 100) bad++;
    end
    o = new;
    ok &= o.randomize();
    o.p = new;
    repeat (20) begin
      ok &= o.randomize();
      if (o.p.v != o.k) bad++;
    end
    $display("%0d %0d", ok, bad);
  end
endmodule

//> 1 0
