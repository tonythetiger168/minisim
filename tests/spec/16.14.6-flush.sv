// IEEE 1800-2023 16.14.6.2: a pending procedural assertion instance is
// flushed when its process resumes again in the same time step (a glitch);
// an instance that matured when its clock did not tick starts at the next tick.
//! icarus-skip: Icarus 13 does not support concurrent assertions
module top;
  logic clk = 0, a = 0, b = 1;
  always #5 clk = ~clk;
  always @(posedge clk or a) begin
    q1: assert property (@(posedge clk) b) $display("%0t q1 pass", $time); else $display("%0t q1 fail", $time);
  end
  initial begin
    @(posedge clk) #0 a = 1;  // 5: the procedure runs again: the first instance is flushed
    #7 a = 0;                 // 12: queued, matures without a tick, starts at 15
    #10 $finish;              // 22
  end
endmodule

//> 5 q1 pass
//> 15 q1 pass
//> 15 q1 pass
