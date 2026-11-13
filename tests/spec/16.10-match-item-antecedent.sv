// IEEE 1800-2023 16.10 / 16.12.7: an overlapped implication whose
// antecedent ends with a local variable assignment, `(a, x = d) |-> p`, is
// decided in the tick where the antecedent matches; it has no later match
// to wait for. The pass action runs in that tick, not one tick later.
// a_f checks a future value: its attempt at 15 sees a fall at 25 and fails
// there; nothing is reported at 5, before any attempt could be decided.
//! icarus-skip: Icarus has no local variables in properties
module t;
  bit clk, a;
  int d;
  global clocking gc @(posedge clk); endclocking
  always #5 clk = !clk;
  property p;
    int x;
    @(posedge clk) (a, x = d) |-> d == x;
  endproperty
  a_p: assert property (p) begin if (a) $display("%0t pass with a", $time); end
  c_p: cover property (p) begin if (a) $display("%0t covered with a", $time); end
  a_f: assert property (@($global_clock) $future_gclk(a)) else $display("%0t future fail", $time);
  initial begin
    @(negedge clk) a = 1;
    @(negedge clk) a = 0;
    @(negedge clk) a = 1;
    #10 $finish;
  end
endmodule
//> 15 pass with a
//> 15 covered with a
//> 25 future fail
//> 35 pass with a
//> 35 covered with a
