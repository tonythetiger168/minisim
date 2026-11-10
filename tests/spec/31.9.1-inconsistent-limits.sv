`timescale 1ns/1ns
// IEEE 1800-2023 31.9.1: limits with no consistent delays (a window from 10
// to 5 after the edge): the most negative limit counts as 0, with a
// warning. A model that clocks its delayed signals latches the data that
// is stable in the window: here the value of D 12 after the edge.
//! icarus-skip: Icarus 13 does not run timing checks
//! args: +neg_tchk
module ff(input C, input D, output reg Q);
  wire dC, dD;
  always @(posedge dC) Q <= dD;
  specify
    $setuphold(posedge C, D, -10, 20, , , , dC, dD);
  endspecify
endmodule
module bad(input C, input D);
  specify
    $setuphold(posedge C, D, -10, 5);
  endspecify
endmodule
module top;
  reg C = 0, D = 0;
  wire Q;
  ff u(C, D, Q);
  bad b(C, D);
  initial begin
    #100 C = 1;
    #3 D = 1;       // 103: the old value would be clocked in without delays
    #30 $display("Q=%b", Q);
    $finish;
  end
endmodule

//> "31.9.1-inconsistent-limits.sv", 17: Timing violation in top.b
//>     $setuphold( posedge C:100, D:103, -10, 5 );
//> Q=1
