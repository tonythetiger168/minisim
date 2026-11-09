`timescale 1ns/1ns
// IEEE 1800-2023 32.4.2: an SDF timing check with no edges or conditions
// annotates every SystemVerilog check of its kind between those signals;
// with an edge or a COND, only the checks whose edge and condition match.
// The three SDF entries are the three examples of 32.4.2, one per instance.
//! icarus-skip: Icarus 13 does not run timing checks
module ff(input clk, data, mode);
  reg ntfr;
  specify
    $setuphold (posedge clk &&& mode, data, 1, 1, ntfr);
    $setuphold (negedge clk &&& !mode, data, 1, 1, ntfr);
    $setuphold (edge clk, data, 1, 1, ntfr);
  endspecify
endmodule
module top;
  reg clk, data, mode = 1;
  ff u1(clk, data, mode);   // (SETUPHOLD data clk (3) (4)): all three
  ff u2(clk, data, mode);   // (SETUPHOLD data (posedge clk) (3) (4)): the first
  ff u3(clk, data, mode);   // (SETUPHOLD data (COND !mode (posedge clk)) (3) (4)): none
  initial begin
    $sdf_annotate("data/sdf/32.4.2-timing-check-match.sdf", top);
    #4 clk = 0;     // clk and data start well apart (x to 0)
    #5 data = 0;
    #9 data = 1;    // 2 before the rising edge at 20: setup 3 is violated, 1 is not
    #2 clk = 1;
    #3 data = 0;    // 3 after it: hold 4 is violated, 1 is not
    #10 $finish;
  end
endmodule

//> "32.4.2-timing-check-match.sv", 10: Timing violation in top.u1
//>     $setuphold( posedge clk &&& mode:20, data:18, 3, 4 );
//> "32.4.2-timing-check-match.sv", 12: Timing violation in top.u1
//>     $setuphold( edge clk:20, data:18, 3, 4 );
//> "32.4.2-timing-check-match.sv", 10: Timing violation in top.u2
//>     $setuphold( posedge clk &&& mode:20, data:18, 3, 4 );
//> "32.4.2-timing-check-match.sv", 10: Timing violation in top.u1
//>     $setuphold( posedge clk &&& mode:20, data:23, 3, 4 );
//> "32.4.2-timing-check-match.sv", 12: Timing violation in top.u1
//>     $setuphold( edge clk:20, data:23, 3, 4 );
//> "32.4.2-timing-check-match.sv", 10: Timing violation in top.u2
//>     $setuphold( posedge clk &&& mode:20, data:23, 3, 4 );
