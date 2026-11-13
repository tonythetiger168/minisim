// IEEE 1800-2023 16.9.4: the global clocking future sampled value functions
// $future_gclk, $rising_gclk, $falling_gclk, $steady_gclk and
// $changing_gclk give the value (or change) at the next tick of the global
// clock. An assertion that uses them is decided at that next tick, so its
// action blocks run one global clock tick after the attempt started.
//! icarus-skip: Icarus has no global clocking future functions
module t;
  bit clk;
  bit a, b;
  global clocking gc @(posedge clk); endclocking
  always #5 clk = !clk;
  // b follows a one global clock tick later
  a_follow: assert property (@($global_clock) $future_gclk(b) == a)
    else $display("%0t follow fail a=%0d", $time, a);
  a_rise: assert property (@($global_clock) $rising_gclk(a) |-> !a)
    else $display("%0t rise fail", $time);
  c_rise: cover property (@($global_clock) $rising_gclk(a)) $display("%0t rising_gclk a", $time);
  c_steady: cover property (@($global_clock) $steady_gclk(a) && a) $display("%0t steady high", $time);
  c_fall: cover property (@($global_clock) $changing_gclk(b) && $falling_gclk(b)) $display("%0t falling b", $time);
  c_past: cover property (@($global_clock) $past_gclk(a) && !a && $future_gclk(a)) $display("%0t dip in a", $time);
  initial begin
    @(negedge clk) a = 1;
    @(negedge clk) b = 1;
    @(negedge clk) ;
    @(negedge clk) a = 0;
    @(negedge clk) b = 0; a = 1;
    @(negedge clk) b = 0;   // b should be 1 here: the attempt at 55 fails
    @(negedge clk) b = 1;
    #20 $finish;
  end
endmodule
//> 15 rising_gclk a
//> 25 steady high
//> 35 steady high
//> 55 rising_gclk a
//> 55 falling b
//> 55 dip in a
//> 65 follow fail a=1
//> 65 steady high
//> 75 steady high
//> 85 steady high
