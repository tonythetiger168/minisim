// IEEE 1800-2023 18.7.1 and 18.5: in obj.randomize() with {...}, local::x
// names the caller's x even when the object has an x of its own (here both
// inherit it from one base class). And an element q[i] whose index i is
// itself random is the element the solver picks for i.
//! icarus-skip: Icarus 13 has no constraints
class Base; rand int x; endclass
class Item extends Base; endclass
class Caller extends Base;
  Item f = new;
  function int run();
    x = 5;
    f.x = 4;
    if (!f.randomize() with { x == local::x; }) return 0;
    if (f.x != 5) return 0;
    if (!f.randomize() with { x == local::x + 1; local::x < 100; }) return 0;
    return f.x == 6;
  endfunction
endclass
module top;
  bit [7:0] q[$];
  bit [7:0] p;
  int i;
  initial begin
    automatic Caller c = new;
    automatic int ok = c.run(), seen = 0;
    repeat (40) begin
      q = {8'h1, 8'h2, 8'h3, 8'h4};
      if (!std::randomize(p, q, i) with { i inside {[0:3]}; p == q[i]; }) ok = 0;
      if (p != q[i]) ok = 0;
      seen |= 1 << i;
    end
    $display("ok=%0d indexes=%b", ok, seen[3:0]);
  end
endmodule
//> ok=1 indexes=1111
