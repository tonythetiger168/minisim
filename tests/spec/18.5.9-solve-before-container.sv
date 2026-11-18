// 18.5.9: solve ... before naming a dynamic array, queue or associative
// array. The order does not change which solutions are legal.
//! icarus-skip: Icarus does not support constraints
class Que;
  rand int unsigned q[$];
  rand int unsigned s;
  constraint c {
    q.size() == 3;
    solve q before s;
    foreach (q[i]) q[i] inside {[1:10]};
    s == q[0] + q[1] + q[2];
  }
endclass
class Assoc;
  rand int unsigned m[string];
  rand int unsigned s;
  function new; m["abc"] = 1; m["def"] = 1; endfunction
  constraint c {
    solve m before s;
    m["abc"] == 2;
    m["def"] == 3;
    s == m["abc"] + m["def"];
  }
endclass
module top;
  Que q = new;
  Assoc a = new;
  int bad;
  initial begin
    repeat (20) begin
      if (!q.randomize() || q.q.size() != 3 || q.s != q.q.sum()) bad++;
      foreach (q.q[i]) if (!(q.q[i] inside {[1:10]})) bad++;
    end
    if (!a.randomize() || a.s != 5) bad++;
    $display("bad %0d", bad);
  end
endmodule
//> bad 0
