// IEEE 1800-2023 18.5.13: Soft constraints hold unless they conflict with
// hard ones; between soft constraints, the later one wins (18.5.13.1), and
// with {...} constraints rank above the class's. disable soft drops the
// soft constraints on a variable (18.5.13.2).
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class S;
    rand bit [7:0] a, b, c;
    constraint d1 { soft a == 5; soft b == 1; soft b == 2; soft c == 7; }
  endclass
  S s;
  int ok = 1, spread;
  initial begin
    s = new;
    ok &= s.randomize();
    $display("%0d %0d %0d", s.a, s.b, s.c);
    ok &= s.randomize() with { a > 10; };
    $display("%0d %0d", s.a > 10, s.b);
    ok &= s.randomize() with { soft a == 9; };
    $display("%0d", s.a);
    repeat (20) begin
      ok &= s.randomize() with { disable soft c; };
      if (s.c != 7) spread++;
    end
    $display("%0d %0d", ok, spread > 10);
  end
endmodule

//> 5 2 7
//> 1 2
//> 9
//> 1 1
