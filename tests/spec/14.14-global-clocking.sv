// IEEE 1800-2023 14.14, 14.15: $global_clock, a clocking event with iff,
// and @(cb.d) waiting on a clockvar's sampled value.
//! icarus-skip: Icarus 13 does not support clocking blocks
module top;
  logic clk = 0, en = 0;
  always #5 clk = ~clk;
  logic [3:0] d = 0;
  global clocking gclk @(posedge clk); endclocking
  clocking cb @(posedge clk iff en);
    input d;
  endclocking
  always @($global_clock) $display("%0t global d=%0d past=%0d", $time, d, $past(d));
  always @(cb) $display("%0t cb d=%0d", $time, cb.d);
  always @(cb.d) $display("%0t cb.d changed to %0d", $time, cb.d);
  initial begin
    #2 d = 1; #10 d = 2; en = 1; #10 d = 3; #10 $finish;
  end
endmodule

//> 5 global d=1 past=0
//> 15 global d=2 past=1
//> 15 cb.d changed to 2
//> 15 cb d=2
//> 25 global d=3 past=2
//> 25 cb.d changed to 3
//> 25 cb d=3
