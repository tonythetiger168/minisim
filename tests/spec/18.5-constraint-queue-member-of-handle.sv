// IEEE 1800-2023 18.5, 18.5.8.1, 18.7, 18.12: a constraint that names a queue
// or associative array property of another object through a handle: its size,
// its elements in a foreach, and the queue as an inside list while it is
// being randomized itself.
//! icarus-skip: Icarus does not support constraints
class Inner;
  rand int q[$];
  int keys[string];
endclass
class Outer;
  rand Inner in;
  rand int pick;
  function new(); in = new; in.keys["a"] = 7; in.keys["b"] = 9; endfunction
  constraint c_size { in.q.size() == 3; }
  constraint c_elems { foreach (in.q[i]) in.q[i] inside {[1:4]}; }
  constraint c_pick { pick inside {in.q}; pick > 1; }
endclass
module t;
  Inner b = new;
  Outer o = new;
  int x, ok = 1;
  initial begin
    b.q = '{10, 20, 30};
    repeat (20) begin
      if (!std::randomize(x) with { x inside {b.q}; }) ok = 0;
      if (!(x inside {10, 20, 30})) ok = 0;
    end
    repeat (20) begin
      if (!o.randomize()) ok = 0;
      if (o.in.q.size() != 3) ok = 0;
      foreach (o.in.q[i]) if (!(o.in.q[i] inside {[1:4]})) ok = 0;
      if (!(o.pick inside {o.in.q}) || o.pick <= 1) ok = 0;
    end
    repeat (5) begin
      if (!std::randomize(x) with { x == o.in.keys.size() + o.in.keys["b"]; }) ok = 0;
      if (x != 11) ok = 0;
    end
    $display("ok %0d", ok);
  end
endmodule
//> ok 1
