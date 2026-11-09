`timescale 1ns/1ns
// IEEE 1800-2023 31.9.1: a $hold on the same signals as a $setuphold with
// a negative setup limit sees the delayed signals too, with its limit
// adjusted by the difference of the delays: 5 + 0 - 11 is below 0, so it
// counts as 0, with a warning, and never reports.
//! icarus-skip: Icarus 13 does not run timing checks
//! args: +neg_tchk
module m(input CLK, input DATA);
  specify
    $setuphold(posedge CLK, DATA, -10, 20);
    $hold(posedge CLK, DATA, 5);
  endspecify
endmodule
module top;
  reg CLK = 0, DATA = 0;
  m u(CLK, DATA);
  initial begin
    #100 CLK = 1;
    #2 DATA = 1;
    #50 $finish;
  end
endmodule
