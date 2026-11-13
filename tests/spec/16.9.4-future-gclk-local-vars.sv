// IEEE 1800-2023 16.9.4 with 16.8.2 and 16.10: an assertion that uses the
// global clocking future sampled value functions may have local variables
// that get their values without match items (those are not allowed with
// future functions): local formal arguments and declared initializers.
// They are read in the same, one tick later, time frame as the rest of
// the assertion.
//! icarus-skip: Icarus has no global clocking future functions
module t;
  bit clk, a;
  int d;
  global clocking gc @(posedge clk); endclocking
  always #5 clk = !clk;
  // when a is about to rise, d goes up by exactly one over the next tick
  property step_on_rise(local input int x);
    @($global_clock) $rising_gclk(a) |-> $future_gclk(d) == x + 1;
  endproperty
  a_step: assert property (step_on_rise(d)) else $display("%0t step fail d=%0d", $time, d);
  // a declared local with an initializer: d one tick later is at most one more
  property small_step;
    int x = d;
    @($global_clock) $future_gclk(d) - x <= 1;
  endproperty
  a_small: assert property (small_step) else $display("%0t small fail d=%0d", $time, d);
  initial begin
    repeat (3) @(negedge clk) d++;
    @(negedge clk) begin a = 1; d++; end      // d goes 3 -> 4 as a rises: holds
    @(negedge clk) a = 0;
    @(negedge clk) begin a = 1; d += 5; end   // 4 -> 9 as a rises: both fail
    @(negedge clk) a = 0;
    #30 $finish;
  end
endmodule
//> 65 step fail d=9
//> 65 small fail d=9
