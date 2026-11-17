// IEEE 1800-2023 18.7 and 18.7.1: in obj.randomize() with {...} called from
// a method, a name is looked up in the object being randomized first, then
// in the caller. A method found in the caller runs on the caller's object,
// and local::f() names the caller's f even when the object has an f too.
// Either call is still virtual: the caller's derived class supplies it.
//! icarus-skip: Icarus 13 has no constraints
class Item;
  rand int v;
  function int step(int a); return a + 10; endfunction
endclass
class Gen;
  Item it = new;
  int base = 3;
  virtual function int pick(); return base; endfunction
  function int step(int a); return a + 1; endfunction
  function int run1(); void'(it.randomize() with { v == pick(); }); return it.v; endfunction
  function int run2(); void'(it.randomize() with { v == local::pick(); }); return it.v; endfunction
  function int run3(); void'(it.randomize() with { v == step(0); }); return it.v; endfunction
  function int run4(); void'(it.randomize() with { v == local::step(0); }); return it.v; endfunction
  function int run5(); void'(it.randomize() with { v == step(local::step(step(0))); }); return it.v; endfunction
endclass
class BigGen extends Gen;
  virtual function int pick(); return base * 100; endfunction
endclass

module top;
  Gen g = new;
  BigGen b = new;
  initial begin
    $display("%0d %0d %0d %0d", g.run1(), g.run2(), b.run1(), b.run2());
    $display("%0d %0d %0d", g.run3(), g.run4(), g.run5());
  end
endmodule
//> 3 3 300 300
//> 10 1 21
