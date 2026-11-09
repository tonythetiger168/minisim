// IEEE 1800-2023 14.4, 14.13: input skews. #1step reads the value from
// before the time step, #0 the value in the observed region, #2 the value two
// time units before the clocking event. @(cb) runs after the update (14.10).
//! icarus-skip: Icarus 13 does not support clocking blocks
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic [3:0] d = 0;
  clocking cb @(posedge clk);
    input #1step a = d;
    input #0 b = d;
    input #2 c = d;
  endclocking
  initial begin
    #4 d = 1;   // 4
    #1 d = 2;   // 5, in the same step as the clocking event
    #8 d = 3;   // 13
    #9 d = 4;   // 22
  end
  always @(cb) $display("%0t a=%0d b=%0d c=%0d", $time, cb.a, cb.b, cb.c);
  initial #30 $finish;
endmodule

//> 5 a=1 b=2 c=0
//> 15 a=3 b=3 c=3
//> 25 a=4 b=4 c=4
