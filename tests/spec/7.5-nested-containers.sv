// IEEE 1800-2023 7.5, 7.8, 7.10: containers of containers. Dynamic arrays
// of dynamic arrays, queues of queues and associative arrays of queues:
// element access and writes, methods on an element, new[] of an element,
// patterns, {q, x} with an element, deep copies on assignment and when
// passed to a function, nested foreach and %p, in a class and in an
// automatic task.
//! icarus-skip: Icarus 13 does not support containers of containers
class C;
  int m[][];
  byte unsigned bq[$][$];
  function void fill();
    m = new[2];
    foreach (m[i]) begin
      m[i] = new[i + 1];
      foreach (m[i][j]) m[i][j] = 10 * i + j;
    end
  endfunction
endclass

function automatic int total(int a[][]);
  int s = 0;
  foreach (a[i, j]) s += a[i][j];
  a[0][0] = 99;   // the caller's array does not change
  return s;
endfunction

task automatic t();
  int loc[$][$];
  loc.push_back('{1});
  loc[0].push_back(2);
  $display("loc %p %0d", loc, loc[0].sum());
endtask

module top;
  int m[][];
  int q[$][$];
  int aq[string][$];
  int m2[][];
  int r[$];
  C c = new;
  initial begin
    m = new[2];
    m[1] = new[3];
    m[1][2] = 5;
    m[0] = '{7, 8};
    $display("%0d %0d %0d %0d %0d", m.size(), m[0].size(), m[1].size(), m[1][2], m[0][1]);
    q.push_back('{1, 2});
    q.push_back('{});
    q[1].push_back(9);
    m2 = '{'{4}};
    q = {q, m2[0]};
    $display("%0d %0d %0d %0d", q.size(), q[0][1], q[1][0], q[2][0]);
    aq["a"].push_back(3);
    aq["b"] = '{1, 1, 1};
    $display("%0d %0d %0d", aq.num(), aq["a"][0], aq["b"].size());
    m2 = m;
    m2[1][2] = 6;
    $display("%0d %0d", m[1][2], m2[1][2]);
    foreach (m[i]) foreach (m[i][j]) $write("%0d ", m[i][j]);
    $display;
    foreach (m[i, j]) $write("%0d:%0d ", i, j);
    $display;
    $display("%p %p", q, aq);
    c.fill();
    $display("%p %0d", c.m, total(c.m));
    $display("%0d", c.m[0][0]);
    c.bq.push_back('{});
    c.bq[0].push_back(8'hff);
    $display("%p %0d", c.bq, c.bq[0][0]);
    t();
    r = c.m[1];
    $display("%p", r);
    c.m[1].delete();
    $display("%p %0d", c.m, c.m[1].size());
    c.m = new[1](c.m);
    $display("%p", c.m);
  end
endmodule

//> 2 2 3 5 8
//> 3 2 9 4
//> 2 3 3
//> 5 6
//> 7 8 0 0 5 
//> 0:0 0:1 1:0 1:1 1:2 
//> '{'{1, 2}, '{9}, '{4}} '{"a":'{3}, "b":'{1, 1, 1}}
//> '{'{0}, '{10, 11}} 21
//> 0
//> '{'{255}} 255
//> loc '{'{1, 2}} 3
//> '{10, 11}
//> '{'{0}, '{}} 0
//> '{'{0}}
