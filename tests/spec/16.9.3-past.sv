// IEEE 1800-2023 16.9.3: $sampled, $changed and $past with a tick count, a
// gating expression and an explicit clock. Before the first tick, the past
// value is the default sampled value.
//! icarus-skip: Icarus 13 does not support sampled value functions
module top;
  logic clk = 0, fclk = 0, en = 0;
  logic [3:0] q = 0;
  always #5 clk = ~clk;
  always #2 fclk = ~fclk;
  always @(posedge clk) q <= q + 1;
  initial begin #22 en = 1; #20 en = 0; end
  always @(posedge clk)
    $display("%0t q=%0d s=%0d p1=%0d p2=%0d pg=%0d c=%0d pf=%0d", $time, q, $sampled(q), $past(q), $past(q, 2),
             $past(q, 1, en), $changed(q), $past(q, 1, , @(posedge fclk)));
  initial #60 $finish;
endmodule

//> 5 q=0 s=0 p1=0 p2=0 pg=0 c=0 pf=0
//> 15 q=1 s=1 p1=0 p2=0 pg=0 c=1 pf=1
//> 25 q=2 s=2 p1=1 p2=0 pg=0 c=1 pf=2
//> 35 q=3 s=3 p1=2 p2=1 pg=2 c=1 pf=3
//> 45 q=4 s=4 p1=3 p2=2 pg=3 c=1 pf=4
//> 55 q=5 s=5 p1=4 p2=3 pg=3 c=1 pf=5
