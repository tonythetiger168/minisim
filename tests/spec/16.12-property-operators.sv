// IEEE 1800-2023 16.12: s_eventually (strong: fails at the end of the
// simulation), accept_on, reject_on, nexttime, until, goto repetition; and
// $assertkill / $assertoff / $asserton on attempts in flight (20.11). The
// clock comes from the default clocking (16.16).
//! icarus-skip: Icarus 13 does not support concurrent assertions
module top;
  logic clk = 0, a = 0, b = 0, c = 0, stop = 0;
  always #5 clk = ~clk;
  bit [0:9] A = 10'b1000100000, B = 10'b0000000100, C = 10'b0001000000, S = 10'b0000001000;
  int i = 0;
  initial begin a = A[0]; b = B[0]; c = C[0]; stop = S[0]; end
  always @(negedge clk) begin i++; a = A[i]; b = B[i]; c = C[i]; stop = S[i]; end
  default clocking cb @(posedge clk); endclocking
  e1: assert property (a |-> s_eventually b) else $display("%0t e1 fail", $time);
  e2: assert property (a |-> accept_on(stop) always !c) else $display("%0t e2 fail", $time);
  e3: assert property (a |-> reject_on(c) ##2 1) else $display("%0t e3 fail", $time);
  e4: assert property (a |-> nexttime[2] !c) else $display("%0t e4 fail", $time);
  e5: assert property (a |-> !b until c) else $display("%0t e5 fail", $time);
  e6: assert property (a |-> (b || c)[->1] ##1 !a) else $display("%0t e6 fail", $time);
  e7: assert property (a |-> s_eventually c) else $display("%0t e7 fail", $time);
  k1: assert property (a |-> ##3 b) else $display("%0t k1 fail", $time);
  k2: assert property (a |-> ##3 b) else $display("%0t k2 fail", $time);
  initial begin #22 $assertkill(0, top.k1); $assertoff(0, top.k2); #10 $asserton; end
  initial #95 $finish;
endmodule

//> 35 e2 fail
//> 35 k2 fail
//> 45 e6 fail
//> 75 e5 fail
//> 95 e7 fail
