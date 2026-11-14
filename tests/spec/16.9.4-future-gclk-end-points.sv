// IEEE 1800-2023 16.9.4: a sequence that uses a global clocking future
// sampled value function, read through .triggered in an assertion. The end
// point is known one global clock tick after the match, so the assertion
// reading it is checked that tick late too (its fail action runs then).
// a_rises matches at a tick where a is 0 and is 1 at the next tick; each
// such tick needs b.
//! icarus-skip: Icarus has no global clocking future functions
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  global clocking @(posedge clk); endclocking
  logic a = 0, b = 0;
  sequence a_rises;
    @(posedge clk) $rising_gclk(a);
  endsequence
  a1: assert property (@(posedge clk) a_rises.triggered |-> b)
    else $display("%0t a1 fail", $time);
  c1: cover property (@(posedge clk) a_rises.triggered) $display("%0t c1 rise seen", $time);
  initial begin
    @(negedge clk) b = 1;          // 15: a rises by the next tick, b is 1
    @(negedge clk) a = 1;          // 25: a is 1
    @(negedge clk) a = 0;          // 35
    @(negedge clk) b = 0;          // 45: a rises by the next tick, b is 0: fails
    @(negedge clk) a = 1;          // 55
    #30 $finish;
  end
endmodule
//> 25 c1 rise seen
//> 55 a1 fail
//> 55 c1 rise seen
