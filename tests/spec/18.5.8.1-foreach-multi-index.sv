// IEEE 1800-2023 18.5.8.1: a foreach constraint with several loop variables
// over a container of containers, foreach (q[i, j]), visits every element of
// every inner container, the same as nested foreach loops. It works over
// queues of dynamic arrays, three levels of dynamic arrays, a fixed-size array
// of queues (the fixed dimension unrolled) and an associative array of queues
// with string keys. A trailing loop variable may be left out (12.7.3).
//! icarus-skip: Icarus 13 does not support classes
class C; rand int q[$][]; constraint c { q.size() == 2; foreach (q[i]) q[i].size() == 3; foreach (q[i, j]) q[i][j] inside {[1:5]}; } endclass
class D;
  rand bit [7:0] t[][][];
  rand int f[2][$];
  rand int a[string][$];
  constraint c { t.size() == 2; foreach (t[i]) t[i].size() == i + 1; foreach (t[i, j]) t[i][j].size() == 2;
                 foreach (t[i, j, k]) t[i][j][k] == i * 100 + j * 10 + k;
                 foreach (f[i]) f[i].size() == i + 2; foreach (f[i, j]) f[i][j] == i * 10 + j;
                 foreach (a[s]) a[s].size() == 2; foreach (a[s, j]) a[s][j] == s.len() + j;
                 foreach (t[i, ]) t[i][0][0] >= 0; }
endclass
module top; C c = new; D d = new; int bad;
initial begin
  d.a["x"] = {}; d.a["yyy"] = {};
  repeat (5) begin
    if (!c.randomize()) bad++;
    foreach (c.q[i, j]) if (!(c.q[i][j] inside {[1:5]})) bad++;
    if (c.q.size() != 2 || c.q[1].size() != 3) bad++;
    if (!d.randomize()) bad++;
  end
  $display("bad %0d", bad);
  $display("%p", d.t); $display("%p", d.f); $display("%p", d.a);
end endmodule

//> bad 0
//> '{'{'{0, 1}}, '{'{100, 101}, '{110, 111}}}
//> '{'{0, 1}, '{10, 11, 12}}
//> '{"x":'{1, 2}, "yyy":'{3, 4}}
