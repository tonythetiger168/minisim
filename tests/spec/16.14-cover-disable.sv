// IEEE 1800-2023 16.12, 16.14: disable iff, overlapped and nonoverlapped
// implication, cover property, and cover sequence (one pass per match end,
// 16.14.3). $assertvacuousoff (20.11) hides vacuous passes.
//! icarus-skip: Icarus 13 does not support concurrent assertions
module top;
  logic clk = 0, rst = 0, a = 0, b = 0;
  always #5 clk = ~clk;
  // values at the posedge of cycle k (time 10k+5): rst, a, b
  bit [0:11] R = 12'b001000000000, A = 12'b010010001000, B = 12'b000100000010;
  int i = 0;
  initial begin rst = R[0]; a = A[0]; b = B[0]; end
  always @(negedge clk) begin i++; rst = R[i]; a = A[i]; b = B[i]; end
  initial $assertvacuousoff;
  p1: assert property (@(posedge clk) disable iff (rst) a |-> ##[1:2] b)
        $display("%0t p1 pass", $time); else $display("%0t p1 fail", $time);
  p2: assert property (@(posedge clk) a |=> b) else $display("%0t p2 fail", $time);
  c1: cover property (@(posedge clk) a ##[1:3] b) $display("%0t c1 covered", $time);
  c2: cover sequence (@(posedge clk) a ##[1:3] b[->1]) $display("%0t c2 match", $time);
  initial #120 $finish;
endmodule

//> 25 p2 fail
//> 35 c1 covered
//> 35 c2 match
//> 55 p2 fail
//> 65 p1 fail
//> 95 p2 fail
//> 105 p1 pass
//> 105 c1 covered
//> 105 c2 match
//> 105 c2 match
//> 105 c2 match
