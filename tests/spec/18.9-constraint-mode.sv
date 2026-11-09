// IEEE 1800-2023 18.9: constraint_mode(0) turns a constraint block off,
// constraint_mode(1) back on; obj.constraint_mode(v) sets every block; the
// function form reads the mode.
//! icarus-skip: Icarus 13 does not support constraint_mode
module top;
  class P;
    rand bit [7:0] a;
    constraint below10 { a < 10; }
    constraint even { a % 2 == 0; }
  endclass
  P p;
  int big, odd;
  initial begin
    p = new;
    p.below10.constraint_mode(0);
    repeat (50) begin void'(p.randomize()); if (p.a >= 10) big++; if (p.a % 2) odd++; end
    $display("%0d %0d %0d %0d", big > 30, odd, p.below10.constraint_mode(), p.even.constraint_mode());
    p.constraint_mode(0);
    odd = 0;
    repeat (50) begin void'(p.randomize()); if (p.a % 2) odd++; end
    $display("%0d", odd > 10);
    p.constraint_mode(1);
    big = 0; odd = 0;
    repeat (50) begin void'(p.randomize()); if (p.a >= 10) big++; if (p.a % 2) odd++; end
    $display("%0d %0d", big, odd);
  end
endmodule

//> 1 0 0 1
//> 1
//> 0 0
