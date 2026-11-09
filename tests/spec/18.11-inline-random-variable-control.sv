// IEEE 1800-2023 18.11: randomize(a, b) randomizes just those variables
// (even ones not declared rand) and holds the others; randomize(null)
// changes nothing and reports whether the constraints hold (18.11.1).
//! icarus-skip: Icarus 13 does not support randomize() arguments
module top;
  class C;
    rand bit [7:0] x, y;
    bit [7:0] v;
    constraint c { x < y; v < 20; }
  endclass
  C c;
  int r;
  initial begin
    c = new;
    c.x = 5; c.y = 9; c.v = 0;
    r = c.randomize(v);
    $display("%0d %0d %0d %0d", r, c.x, c.y, c.v < 20);
    r = c.randomize(y) with { y > 100; };
    $display("%0d %0d %0d", r, c.x, c.y > 100);
    c.x = 50; c.y = 10;
    r = c.randomize(null);
    $display("%0d %0d %0d", r, c.x, c.y);
    c.x = 1;
    r = c.randomize(null);
    $display("%0d", r);
  end
endmodule

//> 1 5 9 1
//> 1 5 1
//> 0 50 10
//> 1
