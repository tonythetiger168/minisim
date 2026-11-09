`timescale 1ns/1ns
// IEEE 1800-2023 31.3.3, 31.3.4-31.3.6: $setuphold, $removal, $recovery and
// $recrem. Simultaneous events violate $setuphold and $recrem.
//! icarus-skip: Icarus 13 does not run timing checks
module m(input clk, input d, input rst);
  specify
    $setuphold(posedge clk, d, 3, 2);
    $removal(posedge rst, posedge clk, 4);
    $recovery(posedge rst, posedge clk, 5);
    $recrem(posedge rst, posedge clk, 5, 4);
  endspecify
endmodule
module top;
  reg clk = 0, d = 0, rst = 0;
  m u(clk, d, rst);
  initial begin
    #8 d = 1;          // setup: 2 before the edge at 10
    #2 clk = 1;        // 10
    #1 d = 0;          // 11: hold
    #9 clk = 0;        // 20
    #10 d = 1; clk = 1; // 30: both at once
    #5 clk = 0;        // 35
    #2 rst = 1;        // 37
    #3 clk = 1;        // 40: recovery (3 after rst)
    #10 clk = 0;       // 50
    #8 clk = 1;        // 58
    #1 rst = 0;        // 59
    #1 rst = 1;        // 60: removal (2 after clk at 58)
    #20 $finish;
  end
endmodule

//> "31.3-setuphold-recrem.sv", 7: Timing violation in top.u
//>     $setuphold( posedge clk:10, d:8, 3, 2 );
//> "31.3-setuphold-recrem.sv", 7: Timing violation in top.u
//>     $setuphold( posedge clk:10, d:11, 3, 2 );
//> "31.3-setuphold-recrem.sv", 7: Timing violation in top.u
//>     $setuphold( posedge clk:30, d:30, 3, 2 );
//> "31.3-setuphold-recrem.sv", 9: Timing violation in top.u
//>     $recovery( posedge rst:37, posedge clk:40, 5 );
//> "31.3-setuphold-recrem.sv", 10: Timing violation in top.u
//>     $recrem( posedge rst:37, posedge clk:40, 5, 4 );
//> "31.3-setuphold-recrem.sv", 8: Timing violation in top.u
//>     $removal( posedge rst:60, posedge clk:58, 4 );
//> "31.3-setuphold-recrem.sv", 10: Timing violation in top.u
//>     $recrem( posedge rst:60, posedge clk:58, 5, 4 );
