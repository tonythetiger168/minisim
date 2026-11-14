// IEEE 1800-2023 18.5.1, 18.5.2: a pure constraint in a virtual class is a
// prototype that a derived class overrides; an implicit extern constraint
// with no body is empty.
virtual class base;
  rand int v;
  pure constraint c;
endclass
class d extends base;
  constraint c { v == 5; }
endclass
class e;
  rand bit [3:0] x;
  constraint none;
  constraint lim { x < 3; }
endclass
module t;
  d o = new;
  e p = new;
  initial begin
    if (!o.randomize() || !p.randomize()) $display("randomize failed");
    $display("%0d %0d", o.v, p.x < 3);
  end
endmodule
//> 5 1
//! icarus-skip: Icarus has no constrained randomization
