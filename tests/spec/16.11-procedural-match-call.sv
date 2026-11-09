// IEEE 1800-2023 16.11, 16.14.6.1: a subroutine call in a match item of a
// procedural assertion sees automatic variables as they were when queued.
//! icarus-skip: Icarus 13 does not support concurrent assertions
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic a = 0;
  int d = 0, q = 0;
  always @(posedge clk) q <= d;
  always @(posedge clk) begin
    automatic int k = d * 2;
    assert property (a |=> ##0 (q == k / 2, $display("%0t q=%0d k=%0d", $time, q, k)) ##0 1);
  end
  initial begin
    @(negedge clk) a = 1; d = 3;
    @(negedge clk) a = 0; d = 4;
    @(negedge clk) $finish;
  end
endmodule

//> 25 q=3 k=6
