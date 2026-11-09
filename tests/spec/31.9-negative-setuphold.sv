`timescale 1ns/1ns
// IEEE 1800-2023 31.9: with +neg_tchk, $setuphold(posedge CLK, DATA, -10, 20)
// checks the window from 10 to 20 after the clock edge, on delayed copies
// of CLK (delayed 11) and DATA. The notifier toggles when the delayed
// signals violate (31.9.3). $recrem works the same way.
//! icarus-skip: Icarus 13 does not run timing checks
//! args: +neg_tchk
module m(input CLK, input DATA, input RST);
  reg ntfr;
  specify
    $setuphold(posedge CLK, DATA, -10, 20, ntfr);
    $recrem(posedge RST, posedge CLK, -2, 6);
  endspecify
  always @(ntfr) $display("%0t ntfr=%b", $time, ntfr);
endmodule
module top;
  reg CLK = 0, DATA = 0, RST = 0;
  m u(CLK, DATA, RST);
  initial begin
    #100 CLK = 1;
    #5 DATA = 1;     // 105: 5 after the edge, before the window
    #10 DATA = 0;    // 115: inside the window
    #10 DATA = 1;    // 125: after it
    #25 CLK = 0;     // 150
    #46 CLK = 1;     // 196: 4 before RST, inside the removal window
    #4 RST = 1;      // 200
    #10 RST = 0; CLK = 0;
    #10 RST = 1;     // 220
    #1 CLK = 1;      // 221: the recovery limit is -2, so this is fine
    #100 $finish;
  end
endmodule

//> "31.9-negative-setuphold.sv", 11: Timing violation in top.u
//>     $setuphold( posedge CLK:100, DATA:115, -10, 20 );
//> 115 ntfr=1
//> "31.9-negative-setuphold.sv", 12: Timing violation in top.u
//>     $recrem( posedge RST:200, posedge CLK:196, -2, 6 );
