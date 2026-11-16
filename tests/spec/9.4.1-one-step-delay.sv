// 9.4.1, 3.14.3: #1step as a procedural delay waits one unit of the global
// time precision (the finest precision in the design), here 1 ps, whatever
// the module's own time unit.
//! icarus-skip: Icarus 13 cannot parse #1step
`timescale 1ns / 1ns
module other;
  timeunit 1ps; timeprecision 1ps;
endmodule
module top;
  other o();
  initial begin
    #1;
    $display("%0.3f", $realtime);
    repeat (10) #1step;
    $display("%0.3f", $realtime);
    #1step $display("%0.3f", $realtime);
  end
endmodule
//> 1.000
//> 1.010
//> 1.011
