// IEEE 1800-2023 37 (timing check objects), 38.10 and 38.36.1:
// vpi_iterate(vpiTchk, module) gives the timing checks of a module
// instance with vpiTchkType and line; vpiTchkRefTerm / vpiTchkDataTerm give
// their terms (vpiEdge, vpiExpr: the signal) and vpiTchkNotifier the
// notifier; vpi_get_delays / vpi_put_delays read and write the limits (and
// the violation report shows the new ones); cbTchkViolation runs after
// each violation.
//! icarus-skip: Icarus 13 has no timing check objects
//! args: data/vpi/tchk.c
`timescale 1ns/1ns
module dff(input d, input clk, output reg q);
  reg ntfr;
  always @(posedge clk) q <= d;
  specify
    $setup(d, posedge clk, 2, ntfr);
    $hold(posedge clk, d, 1);
    $setuphold(posedge clk, negedge d, 2, 1);
    $width(posedge clk, 3);
  endspecify
endmodule
module top;
  reg d = 0, clk = 0;
  wire q;
  dff ff(.d(d), .clk(clk), .q(q));
  initial begin
    $tchks;
    #10 d = 1;
    #3 clk = 1;    // d changed 3 before: no setup violation with 2, one with 4
    #5 clk = 0;
    #20 $finish;
  end
endmodule
//> vpiTchk type 1 line 15: ref posedge clk edge 13 (top.ff.clk) data d edge 0 notifier top.ff.ntfr limits 2 0
//> vpiTchk type 2 line 16: ref posedge clk edge 13 (top.ff.clk) data d edge 0 limits 1 0
//> vpiTchk type 8 line 17: ref posedge clk edge 13 (top.ff.clk) data negedge d edge 50 limits 2 1
//> vpiTchk type 4 line 18: ref posedge clk edge 13 (top.ff.clk) limits 3 0
//> setup now: limits 4 0
//> "38.36-vpi-tchk.sv", 15: Timing violation in top.ff
//>     $setup( d:10, posedge clk:13, 4 );
//>   cbTchkViolation at 13: type 1 line 15
