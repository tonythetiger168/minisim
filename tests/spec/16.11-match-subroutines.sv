// IEEE 1800-2023 16.11: system and user subroutine calls in match items,
// with local variables as arguments, run when the sequence matches.
//! icarus-skip: Icarus 13 does not support assertion local variables
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic a = 0, b = 0;
  logic [7:0] e = 0, f = 0;
  int n = 0;
  task automatic note(input logic [7:0] x);
    n += x;
  endtask
  // 16.11: subroutine calls on a match, with local variables as arguments
  sequence s1;
    logic [7:0] v, w;
    (a, v = e, $display("%0t a: v = %h", $time, v)) ##1 (b[->1], w = f, $display("%0t b after a: v = %h, w = %h", $time, v, w), note(v + w));
  endsequence
  c1: cover property (@(posedge clk) s1);
  initial begin
    @(negedge clk) a = 1; e = 8'h12;
    @(negedge clk) a = 0; e = 0;
    @(negedge clk) b = 1; f = 8'h34;
    @(negedge clk) b = 0;
    @(negedge clk) $display("n = %0d", n);
    $finish;
  end
endmodule

//> 15 a: v = 12
//> 35 b after a: v = 12, w = 34
//> n = 70
