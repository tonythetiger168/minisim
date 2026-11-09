// IEEE 1800-2023 17.3: a checker with an event formal and an output, set
// by the action blocks of its assertion (the mutex example). The output
// changes in the reactive region, after the module's always block reads it.
//! icarus-skip: Icarus 13 has no checkers
checker mutex (logic [3:0] sig, event clock, output bit failure);
  a1: assert property (@clock $onehot0(sig)) failure = 1'b0; else failure = 1'b1;
endchecker : mutex
module m;
  logic [3:0] bus = 0;
  logic clk = 0, res, scan;
  mutex check_bus(bus, posedge clk, res);
  always @(posedge clk) scan <= res;
  always #5 clk = ~clk;
  initial begin
    #12 bus = 4'b0011;
    #10 bus = 4'b0100;
    #10 $display("res=%b scan=%b", res, scan);
    $finish;
  end
endmodule
//> res=0 scan=1
