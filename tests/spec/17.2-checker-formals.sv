// IEEE 1800-2023 17.2 (Example 4), 17.5: a checker with a default input, an
// initialized output, and an always_ff that counts on sampled values. Its
// nonblocking assignments land in the Re-NBA region, after the action block.
//! icarus-skip: Icarus 13 has no checkers
checker my_check4 (input logic in,
                   en = 1'b1,
                   event clock,
                   output int ctr = 0);
  default clocking @clock; endclocking
  always_ff @clock
    if (en && in) ctr <= ctr + 1;
  a1: assert property (ctr < 3) else $display("%0t a1 failed: ctr=%0d", $time, ctr);
endchecker : my_check4

module top;
  logic clk = 0, x = 0;
  int n;
  always #5 clk = ~clk;
  // x changes at the clock edge: the checker sees its sampled value
  always @(posedge clk) x <= 1;
  my_check4 c(x, , posedge clk, n);
  initial begin
    #1 $display("n=%0d", n);
    #50 $display("n=%0d", n);
    $finish;
  end
endmodule
//> n=0
//> 45 a1 failed: ctr=3
//> n=4
