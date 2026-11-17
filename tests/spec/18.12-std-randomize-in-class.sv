// IEEE 1800-2023 18.12 and 18.11: inside a class method, std::randomize()
// is scope randomization: it needs no object, so it also works in a static
// method, and its with {...} may read the class's static and non-static
// properties. A plain randomize(v) in a method is this.randomize(v) and
// changes only v.
//! icarus-skip: Icarus 13 has no constraints
module top;
  class Pool;
    static int allowed[$];
    int lim = 10;
    rand int r;
    int p;
    static function int pick();
      int n, ok;
      ok = std::randomize(n) with { n inside {allowed}; };
      return ok ? n : -1;
    endfunction
    function int above();
      int n, ok;
      ok = std::randomize(n) with { n > lim; n < lim + 2; };
      return n;
    endfunction
    function int prop();
      void'(std::randomize(p) with { p == lim * 3; });
      return p;
    endfunction
    function int own();
      r = 5;
      void'(randomize(p) with { p == r + 1; });
      return p * 100 + r;
    endfunction
  endclass
  Pool o = new;
  initial begin
    automatic int good = 1;
    Pool::allowed = '{3, 8, 21};
    repeat (20) if (!(Pool::pick() inside {3, 8, 21})) good = 0;
    $display("static %0d", good);
    $display("%0d %0d %0d", o.above(), o.prop(), o.own());
  end
endmodule
//> static 1
//> 11 30 605
