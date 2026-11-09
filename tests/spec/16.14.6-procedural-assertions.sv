// IEEE 1800-2023 16.14.6: concurrent assertions in a procedure. The clock is
// inferred from the always procedure; an instance is queued only when the
// procedure reaches it, and automatic variables (the loop index) are read
// as they were then, in the property and in the action block (16.14.6.1).
//! icarus-skip: Icarus 13 does not support concurrent assertions
module top;
  logic clk=0, a=0, b=0, en=0;
  always #5 clk = ~clk;
  always @(posedge clk) begin
    if (en) begin
      p1: assert property (a |=> b) $display("%0t p1 pass", $time); else $display("%0t p1 fail", $time);
    end
  end
  // automatic capture: loop variable read as queued
  logic [3:0] v = 0;
  always @(posedge clk) begin
    for (int i = 0; i < 4; i++) begin
      c1: cover property (v[i]) $display("%0t c1 i=%0d", $time, i);
    end
  end
  initial begin
    @(negedge clk); en=1; a=1;      // queued at 15 (a=1) -> check b at 25
    @(negedge clk); a=0; b=1; v=4'b0101;
    @(negedge clk); en=0; b=0; a=1;
    @(negedge clk); en=1; a=1; b=0;  // at 45 a=1, 55 b=0 fail
    @(negedge clk); a=0; v=0;
    @(negedge clk); @(negedge clk);
    $finish;
  end
endmodule

//> 25 p1 pass
//> 25 p1 pass
//> 25 c1 i=0
//> 25 c1 i=2
//> 35 c1 i=0
//> 35 c1 i=2
//> 45 c1 i=0
//> 45 c1 i=2
//> 55 p1 fail
//> 55 p1 pass
//> 65 p1 pass
