// IEEE 1800-2023 18.8: rand_mode(0) makes a random variable a state
// variable (randomize() leaves it alone and constraints treat it as a
// constant); rand_mode(1) turns it back on; obj.rand_mode(v) sets every
// variable of the object; x.rand_mode() reads the mode.
//! icarus-skip: Icarus 13 does not support rand_mode
module top;
  class P;
    rand bit [7:0] a, b;
    constraint c { b == a + 1; }
  endclass
  P p;
  initial begin
    p = new;
    p.a.rand_mode(0);
    p.a = 40;
    void'(p.randomize());
    $display("%0d %0d %0d %0d", p.a, p.b, p.a.rand_mode(), p.b.rand_mode());
    p.rand_mode(0);
    p.a = 1; p.b = 2;
    void'(p.randomize());
    $display("%0d %0d %0d", p.a, p.b, p.b.rand_mode());
    p.rand_mode(1);
    void'(p.randomize());
    $display("%0d", p.b == p.a + 1);
  end
endmodule

//> 40 41 0 1
//> 1 2 0
//> 1
