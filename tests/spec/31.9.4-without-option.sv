`timescale 1ns/1ns
// IEEE 1800-2023 31.9.4: without the negative timing check option, a
// negative limit counts as 0 and the delayed signals are plain copies.
//! icarus-skip: Icarus 13 does not run timing checks
module m(input CLK, input DATA);
  wire dCLK, dDATA;
  specify
    $setuphold(posedge CLK, DATA, -10, 20, , , , dCLK, dDATA);
  endspecify
  always @(dCLK) $display("%0t dCLK=%b", $time, dCLK);
endmodule
module top;
  reg CLK = 0, DATA = 0;
  m u(CLK, DATA);
  initial begin
    #100 CLK = 1;
    #15 DATA = 1;
    #50 $finish;
  end
endmodule

//> 0 dCLK=0
//> 100 dCLK=1
//> "31.9.4-without-option.sv", 8: Timing violation in top.u
//>     $setuphold( posedge CLK:100, DATA:115, -10, 20 );
