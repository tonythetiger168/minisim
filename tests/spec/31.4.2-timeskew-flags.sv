`timescale 1ns/1ns
// IEEE 1800-2023 31.4.2, Figure 31-1: $timeskew with each combination of
// event_based_flag and remain_active_flag. The reference event at F (120)
// comes while MODE is 0.
//! icarus-skip: Icarus 13 does not run timing checks
module m(input CP, input CPN, input MODE);
  specify
    $timeskew(posedge CP &&& MODE, negedge CPN, 50);            // case 1
    $timeskew(posedge CP &&& MODE, negedge CPN, 50, , 1, 0);    // case 2
    $timeskew(posedge CP &&& MODE, negedge CPN, 50, , 1, 1);    // case 3
    $timeskew(posedge CP &&& MODE, negedge CPN, 50, , 0, 1);    // case 4
  endspecify
endmodule
module top;
  reg CP = 0, CPN = 1, MODE = 1;
  m u(CP, CPN, MODE);
  initial begin
    #10 CP = 1;                    // A
    #50 CP = 0;
    #10 CPN = 0; #10 CPN = 1;      // C at 70
    #10 CPN = 0; #10 CPN = 1;      // D at 90
    #10 CPN = 0; #5 MODE = 0;      // E at 110
    #5 CP = 1; CPN = 1;            // F at 120
    #10 CPN = 0; #10 CPN = 1;      // G at 130
    #10 CPN = 0; #10 CPN = 1;      // H at 150
    #10 CPN = 0; #10 CPN = 1;      // I at 170
    #10 CPN = 0;                   // J at 190
    #10 $finish;
  end
endmodule

//> "31.4.2-timeskew-flags.sv", 8: Timing violation in top.u
//>     $timeskew( posedge CP &&& MODE:10, negedge CPN:60, 50 );
//> "31.4.2-timeskew-flags.sv", 11: Timing violation in top.u
//>     $timeskew( posedge CP &&& MODE:10, negedge CPN:60, 50 );
//> "31.4.2-timeskew-flags.sv", 9: Timing violation in top.u
//>     $timeskew( posedge CP &&& MODE:10, negedge CPN:70, 50 );
//> "31.4.2-timeskew-flags.sv", 10: Timing violation in top.u
//>     $timeskew( posedge CP &&& MODE:10, negedge CPN:70, 50 );
//> "31.4.2-timeskew-flags.sv", 10: Timing violation in top.u
//>     $timeskew( posedge CP &&& MODE:10, negedge CPN:90, 50 );
//> "31.4.2-timeskew-flags.sv", 10: Timing violation in top.u
//>     $timeskew( posedge CP &&& MODE:10, negedge CPN:110, 50 );
//> "31.4.2-timeskew-flags.sv", 10: Timing violation in top.u
//>     $timeskew( posedge CP &&& MODE:10, negedge CPN:130, 50 );
//> "31.4.2-timeskew-flags.sv", 10: Timing violation in top.u
//>     $timeskew( posedge CP &&& MODE:10, negedge CPN:150, 50 );
//> "31.4.2-timeskew-flags.sv", 10: Timing violation in top.u
//>     $timeskew( posedge CP &&& MODE:10, negedge CPN:170, 50 );
//> "31.4.2-timeskew-flags.sv", 10: Timing violation in top.u
//>     $timeskew( posedge CP &&& MODE:10, negedge CPN:190, 50 );
