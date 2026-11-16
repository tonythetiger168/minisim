// IEEE 1800-2023 11.4.13: in a range of an inside list, $ stands for the
// lowest value (left bound) or the highest value (right bound) of the
// left-hand operand, so [$:hi] and [lo:$] are open ranges.
//! icarus-skip: Icarus 13 does not accept $ in an inside range
class Pick;
  rand bit [7:0] v;
  constraint c { v inside {[250:$]}; }
endclass

module top;
  int x;
  real r;
  int calls;
  function bit [7:0] next(); calls++; return 8'd7; endfunction
  bit [3:0] n;
  initial begin
    automatic Pick p = new;
    for (int k = 0; k < 5; k++) begin
      x = (k - 2) * 60;
      $display("%0d %b %b %b", x, x inside {[$:0]}, x inside {[60:$]}, x inside {[$:-100], [100:$]});
    end
    r = 2.5;
    $display("%b %b", r inside {[$:2.5]}, r inside {[3.0:$]});
    n = 4'd9;
    case (n) inside
      [$:4]: $display("low");
      [10:$]: $display("high");
      default: $display("mid");
    endcase
    n = 4'd15;
    case (n) inside
      [$:4]: $display("low");
      [10:$]: $display("high");
      default: $display("mid");
    endcase
    // the left operand is evaluated once, however many bounds read it
    $display("%b %b %0d", next() inside {[0:$]}, next() inside {[1:3], [5:9]}, calls);
    x = 9;
    $display("%b %0d", ++x inside {[$:10]}, x);
    repeat (4) begin
      void'(p.randomize());
      $display("%b", p.v >= 250);
    end
  end
endmodule
//> -120 1 0 1
//> -60 1 0 0
//> 0 1 0 0
//> 60 0 1 0
//> 120 0 1 1
//> 1 0
//> mid
//> high
//> 1 1 2
//> 1 10
//> 1
//> 1
//> 1
//> 1
