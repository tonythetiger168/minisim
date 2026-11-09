// IEEE 1800-2023 18.11: randomize() with arguments that are containers and
// arrays: a non-rand queue made random for one call, a fixed-size array and a
// queue together, a fixed-size array of queues, and a 2-D dynamic array with
// a scalar. Only the arguments change; every constraint still holds, also on
// the variables that keep their values.
//! icarus-skip: Icarus 13 does not support constraints
class C;
  rand bit [3:0] q[$]; bit [3:0] nq[$]; rand bit [3:0] fa[3]; rand bit [3:0] r[2][$]; rand int x; int m[][];
  constraint c { q.size() inside {[2:4]}; foreach (q[i]) q[i] > 5; nq.size() == 3; foreach (nq[i]) nq[i] == i + 1;
                 foreach (fa[i]) fa[i] == i * 2; foreach (r[i]) r[i].size() == 2; x > 5; m.size() == 2; foreach (m[i]) m[i].size() == i + 1; }
endclass
module top; C c = new; int ok = 1;
  initial begin
    c.x = 6; c.q = '{6, 7}; c.fa = '{0, 2, 4}; c.r[0] = '{1, 1}; c.r[1] = '{2, 2}; c.m = new[2]; c.m[1] = new[2];
    c.m[0] = new[1];
    ok &= c.randomize(nq);   // only nq is random; the rest keep their values
    $display("%p %p %0d %0d", c.nq, c.q, c.x, ok);
    ok &= c.randomize(fa, q);
    $display("%p %0d %0d", c.fa, c.q.size() >= 2 && c.q[0] > 5, c.x);
    ok &= c.randomize(r);
    $display("%0d %0d", c.r[0].size(), c.r[1].size());
    ok &= c.randomize(m, x);
    $display("%0d %0d %0d %0d", c.m.size(), c.m[1].size(), c.x > 5, ok);
  end
endmodule

//> '{1, 2, 3} '{6, 7} 6 1
//> '{0, 2, 4} 1 6
//> 2 2
//> 2 2 1 1
