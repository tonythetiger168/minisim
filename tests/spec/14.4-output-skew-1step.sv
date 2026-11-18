// 14.4, 3.14.3: a clocking output skew of #1step drives the signal one global
// time step (the finest precision) after the clocking event.
//! icarus-skip: Icarus does not support clocking blocks
`timescale 1ns / 1ps
module top;
  bit clk;
  bit y;
  clocking cb @(posedge clk);
    output #1step y;
  endclocking
  initial begin
    #10 clk = 1;
    cb.y <= 1;
    #0.0004 $display("%.4f y=%0d", $realtime, y);
    #0.002 $display("%.4f y=%0d", $realtime, y);
  end
  always @(y) $display("y changed to %0d at %.4f", y, $realtime);
endmodule
//> 10.0000 y=0
//> y changed to 1 at 10.0010
//> 10.0020 y=1
