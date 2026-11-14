// IEEE 1800-2023 18.6.3, 18.8: a static rand property is one variable for
// every object; randomize() of any object solves it with that object's
// constraints, and its rand_mode is shared too.
class a;
  static rand int x;
  rand int y;
  constraint c { x > 0; x < 12; y == x + 1; }
endclass
module t;
  a o1 = new, o2 = new;
  int bad = 0;
  initial begin
    repeat (6) begin
      if (!o1.randomize()) bad++;
      if (o1.y != a::x + 1 || a::x < 1 || a::x > 11 || o2.x != a::x) bad++;
    end
    o1.x.rand_mode(0);
    $display("bad %0d modes %0d %0d", bad, o1.x.rand_mode(), o2.x.rand_mode());
    a::x = 5;
    if (!o2.randomize()) bad++;
    $display("x %0d y %0d bad %0d", a::x, o2.y, bad);
    o2.x.rand_mode(1);
    $display("mode %0d", o1.x.rand_mode());
  end
endmodule
//> bad 0 modes 0 0
//> x 5 y 6 bad 0
//> mode 1
//! icarus-skip: Icarus has no constrained randomization
