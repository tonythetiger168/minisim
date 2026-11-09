`timescale 1ns/1ns
// IEEE 1800-2023 31.4.1-31.4.3: $skew is event-based and checks every data
// event; $timeskew reports when the limit elapses (timer-based) unless the
// event-based flag is set; $fullskew checks both orders.
//! icarus-skip: Icarus 13 does not run timing checks
module m(input a, input b);
  specify
    $skew(posedge a, posedge b, 5);
    $timeskew(posedge a, posedge b, 5);
    $timeskew(posedge a, posedge b, 5, , 1);
    $fullskew(posedge a, posedge b, 5, 3);
    $fullskew(posedge a, posedge b, 5, 3, , 1);
  endspecify
endmodule
module top;
  reg a = 0, b = 0;
  m u(a, b);
  initial begin
    #10 a = 1;          // 10
    #4 b = 1;           // 14: within 5
    #1 b = 0;
    #5 b = 1;           // 20: 10 after a
    #1 b = 0;
    #4 b = 1;           // 25
    #1 a = 0; b = 0;
    #10 b = 1;          // 36: b first
    #2 a = 1;           // 38: 2 after b, within 3
    #2 b = 0; a = 0;
    #10 b = 1;          // 50
    #10 a = 1;          // 60: 10 after b
    #20 $finish;
  end
endmodule

//> "31.4-skew.sv", 8: Timing violation in top.u
//>     $skew( posedge a:10, posedge b:20, 5 );
//> "31.4-skew.sv", 10: Timing violation in top.u
//>     $timeskew( posedge a:10, posedge b:20, 5 );
//> "31.4-skew.sv", 11: Timing violation in top.u
//>     $fullskew( posedge a:23, posedge b:20, 5, 3 );
//> "31.4-skew.sv", 8: Timing violation in top.u
//>     $skew( posedge a:10, posedge b:25, 5 );
//> "31.4-skew.sv", 11: Timing violation in top.u
//>     $fullskew( posedge a:28, posedge b:25, 5, 3 );
//> "31.4-skew.sv", 8: Timing violation in top.u
//>     $skew( posedge a:10, posedge b:36, 5 );
//> "31.4-skew.sv", 9: Timing violation in top.u
//>     $timeskew( posedge a:38, posedge b:43, 5 );
//> "31.4-skew.sv", 8: Timing violation in top.u
//>     $skew( posedge a:38, posedge b:50, 5 );
//> "31.4-skew.sv", 10: Timing violation in top.u
//>     $timeskew( posedge a:38, posedge b:50, 5 );
//> "31.4-skew.sv", 11: Timing violation in top.u
//>     $fullskew( posedge a:53, posedge b:50, 5, 3 );
//> "31.4-skew.sv", 12: Timing violation in top.u
//>     $fullskew( posedge a:60, posedge b:50, 5, 3 );
//> "31.4-skew.sv", 9: Timing violation in top.u
//>     $timeskew( posedge a:60, posedge b:65, 5 );
//> "31.4-skew.sv", 11: Timing violation in top.u
//>     $fullskew( posedge a:60, posedge b:65, 5, 3 );
