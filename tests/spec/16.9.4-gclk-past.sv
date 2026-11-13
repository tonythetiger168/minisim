// IEEE 1800-2023 16.9.4: the global clocking past sampled value functions
// $past_gclk, $rose_gclk, $fell_gclk, $stable_gclk and $changed_gclk are
// the 16.9.3 functions on @$global_clock, in assertions and in procedural
// code.
//! icarus-skip: Icarus 13 has no global clocking
module top;
  logic clk = 0, fast = 0;
  logic [3:0] v = 0;
  logic b = 0;
  global clocking gc @(posedge fast); endclocking
  always #1 fast = ~fast;
  always #4 clk = ~clk;
  initial begin
    #3 b = 1; v = 5;
    #4 b = 0;
    #4 v = 6;
    #10 $finish;
  end
  // b rises between global ticks 2 and 4; seen on the slower clk too.
  a_rose: assert property (@(posedge clk) $rose_gclk(b) |-> v == 5)
    else $display("%0t: a_rose failed", $time);
  always @(posedge fast)
    $display("%0t: past=%0d rose=%b fell=%b stable=%b changed=%b", $time,
             $past_gclk(v), $rose_gclk(b), $fell_gclk(b), $stable_gclk(v), $changed_gclk(v));
endmodule
//> 1: past=0 rose=0 fell=0 stable=1 changed=0
//> 3: past=0 rose=0 fell=0 stable=1 changed=0
//> 5: past=0 rose=1 fell=0 stable=0 changed=1
//> 7: past=5 rose=0 fell=0 stable=1 changed=0
//> 9: past=5 rose=0 fell=1 stable=1 changed=0
//> 11: past=5 rose=0 fell=0 stable=1 changed=0
//> 13: past=5 rose=0 fell=0 stable=0 changed=1
//> 15: past=6 rose=0 fell=0 stable=1 changed=0
//> 17: past=6 rose=0 fell=0 stable=1 changed=0
//> 19: past=6 rose=0 fell=0 stable=1 changed=0
//> 21: past=6 rose=0 fell=0 stable=1 changed=0
