// IEEE 1800-2023 16.9.4 with 16.10: an assertion that uses the global
// clocking future sampled value functions may also have local variables.
// A local variable is assigned and read in the same time frame as the
// rest of the assertion, which is decided one global clock tick late.
// (A future function may not appear in the local variable assignment.)
//! icarus-skip: Icarus has no global clocking future functions
module t;
  bit clk, a;
  int d;
  global clocking gc @(posedge clk); endclocking
  always #5 clk = !clk;
  // a local variable keeps d from the attempt's tick; one tick later d
  // has moved by at most one (fails once, when d jumps by five)
  property small_step;
    int x;
    @($global_clock) (1, x = d) |=> d - x <= 1 && $future_gclk(1'b1);
  endproperty
  a_small: assert property (small_step) else $display("%0t small fail d=%0d", $time, d);
  // when a is about to rise, d must go up by exactly one over the next tick
  property step_on_rise;
    int x;
    @($global_clock) ($rising_gclk(a), x = d) |-> $future_gclk(d) == x + 1;
  endproperty
  a_step: assert property (step_on_rise) else $display("%0t step fail d=%0d", $time, d);
  initial begin
    repeat (3) @(negedge clk) d++;
    @(negedge clk) begin a = 1; d++; end   // d goes 3 -> 4 as a rises: holds
    @(negedge clk) a = 0;
    @(negedge clk) begin a = 1; d += 5; end   // 4 -> 9 as a rises: fails
    @(negedge clk) a = 0;
    #30 $finish;
  end
endmodule
//> 65 step fail d=9
//> 75 small fail d=9
