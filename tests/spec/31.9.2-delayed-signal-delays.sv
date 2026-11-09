`timescale 1ns/10ps
// IEEE 1800-2023 31.9.2: the delays of the delayed signals for the example
// of Figure 31-6: dCP 10.01, dD 0, dTI 20.02, dTE 2.02.
//! icarus-skip: Icarus 13 does not run timing checks
//! args: +neg_tchk
module m(input CP, input D, input TI, input TE);
  wire dCP, dD, dTI, dTE;
  reg notifier;
  assign TE_cond_D = (dTE !== 1'b1);
  assign TE_cond_TI = (dTE !== 1'b0);
  assign DXTI_cond = (dTI !== dD);
  specify
    $setuphold(posedge CP, D, -10, 20, notifier, , TE_cond_D, dCP, dD);
    $setuphold(posedge CP, TI, 20, -10, notifier, , TE_cond_TI, dCP, dTI);
    $setuphold(posedge CP, TE, -4, 8, notifier, , DXTI_cond, dCP, dTE);
  endspecify
  always @(dCP) $display("%t dCP=%b", $realtime, dCP);
  always @(dD) $display("%t dD=%b", $realtime, dD);
  always @(dTI) $display("%t dTI=%b", $realtime, dTI);
  always @(dTE) $display("%t dTE=%b", $realtime, dTE);
endmodule
module top;
  reg CP = 0, D = 0, TI = 0, TE = 0;
  m u(CP, D, TI, TE);
  initial begin
    $timeformat(-9, 2, "", 8);
    #100 CP = 1; D = 1; TI = 1; TE = 1;
    #50 $finish;
  end
endmodule

//>     0.00 dD=0
//>     2.02 dTE=0
//>    10.01 dCP=0
//>    20.02 dTI=0
//>   100.00 dD=1
//>   102.02 dTE=1
//>   110.01 dCP=1
//>   120.02 dTI=1
