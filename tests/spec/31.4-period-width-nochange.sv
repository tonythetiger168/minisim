`timescale 1ns/1ns
// IEEE 1800-2023 31.4.4-31.4.6: $period, $width (with a threshold) and $nochange.
//! icarus-skip: Icarus 13 does not run timing checks
module m(input clk, input en, input d);
  specify
    $period(posedge clk, 10);
    $width(posedge clk, 4, 1);
    $nochange(posedge en, d, 0, 0);
  endspecify
endmodule
module top;
  reg clk = 0, en = 0, d = 0;
  m u(clk, en, d);
  initial begin
    #10 clk = 1;   // 10
    #5 clk = 0;    // 15: high for 5, fine
    #3 clk = 1;    // 18: period 8 < 10
    #3 clk = 0;    // 21: high for 3 < 4
    #9 clk = 1;    // 30: period 12, fine
    #1 clk = 0;    // 31: a 1 ns glitch, not above the threshold
  end
  initial begin
    #40 en = 1; d = 1;  // 40: at the leading edge, fine
    #2 d = 0;           // 42: inside the level
    #3 en = 0;          // 45
    #0 d = 1;           // 45: at the trailing edge, fine
    #5 $finish;
  end
endmodule

//> "31.4-period-width-nochange.sv", 6: Timing violation in top.u
//>     $period( posedge clk:10, :18, 10 );
//> "31.4-period-width-nochange.sv", 7: Timing violation in top.u
//>     $width( posedge clk:18, :21, 4, 1 );
//> "31.4-period-width-nochange.sv", 8: Timing violation in top.u
//>     $nochange( posedge en:40, d:42, 0, 0 );
