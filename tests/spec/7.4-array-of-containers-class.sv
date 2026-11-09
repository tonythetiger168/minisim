// IEEE 1800-2023 7.4 with 7.5 / 7.10 and 18.4: fixed-size arrays of
// containers (int q[4][$]) as class properties and automatic variables:
// element methods, foreach over both dimensions, %p, object copy (8.12),
// access through a handle, a fresh array for each recursive call, and rand
// arrays of queues with constraints, inline with, and enum elements.
//! icarus-skip: Icarus 13 does not support classes with queue arrays
typedef enum {RED, GREEN = 5, BLUE = 9} col_t;
class C;
  int q[4][$];
  int m[2][3][];
  rand bit [3:0] r[3][$];
  rand col_t e[2][];
  constraint c { foreach (r[i]) { r[i].size() == i + 1; foreach (r[i][j]) r[i][j] == i + j; }
                 foreach (e[i]) e[i].size() == 2; }
  function void fill;
    foreach (q[i]) q[i].push_back(i * 10);
    q[2].push_back(7);
    m[1][2] = new[3];
    m[1][2][1] = 5;
  endfunction
endclass
module top;
  C c = new, d;
  function automatic int f(int n);
    int a[2][$];
    a[n % 2].push_back(n);
    if (n > 0) void'(f(n - 1));
    return a[0].size() + a[1].size();
  endfunction
  task automatic show(C h);
    h.q[0].push_back(15);
    $display("%0d %0d", h.q[0].size(), h.q[0][$]);
  endtask
  task automatic t;
    int a[2][$];
    a[0] = {1, 2};
    a[1].push_back(3);
    $display("%p %0d", a, a[0].size());
  endtask
  int bad;
  initial begin
    c.fill();
    $display("%p %0d %p", c.q, c.q[2].size(), c.m);
    foreach (c.q[i, j]) $write("%0d:%0d=%0d ", i, j, c.q[i][j]);
    $display;
    d = new c;
    d.q[2][0] = 100;
    $display("%p %0d", d.q, c.q[2][0]);
    $display("%0d", f(3));
    t();
    show(c);
    if (!c.randomize()) $display("fail");
    $display("%p", c.r);
    foreach (c.e[i, j]) if (!(c.e[i][j] inside {RED, GREEN, BLUE})) bad++;
    if (!c.randomize() with { r[0].size() == 1; }) $display("fail");
    $display("%0d %0d", c.e[1].size(), bad);
  end
endmodule

//> '{'{0}, '{10}, '{20, 7}, '{30}} 2 '{'{'{}, '{}, '{}}, '{'{}, '{}, '{0, 5, 0}}}
//> 0:0=0 1:0=10 2:0=20 2:1=7 3:0=30 
//> '{'{0}, '{10}, '{100, 7}, '{30}} 20
//> 1
//> '{'{1, 2}, '{3}} 2
//> 2 15
//> '{'{0}, '{1, 2}, '{2, 3, 4}}
//> 2 0
