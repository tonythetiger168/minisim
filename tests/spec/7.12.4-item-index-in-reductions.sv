// IEEE 1800-2023 7.12.4 and 18.5.8.2: item.index in the with clause of a
// reduction method gives the element's index: for a fixed-size array its
// declared index (so [3:0] counts down), for a dynamic array or queue its
// position. It may also be used in a constraint's reduction.
//! icarus-skip: Icarus 13 has no reduction with clauses
class C;
  rand bit [3:0] d[5];
  rand int q[];
  constraint c {
    d.sum() with (int'(item) * item.index) == 20;
    q.size() == 4;
    foreach (q[i]) q[i] inside {[0:9]};
    q.sum() with (item * int'(item.index == 2)) == 7;
  }
endclass
module top;
  int up[4] = '{5, 6, 7, 8};
  int down[3:0] = '{5, 6, 7, 8};
  int dq[] = '{1, 2, 3};
  C o = new;
  initial begin
    $display("%0d %0d %0d", up.sum() with (item * item.index), down.sum() with (item * item.index),
             dq.sum() with (item * item.index));
    $display("%0d %0d %0d", o.randomize(), o.d[1] + 2 * o.d[2] + 3 * o.d[3] + 4 * o.d[4], o.q[2]);
  end
endmodule
//> 44 34 8
//> 1 20 7
