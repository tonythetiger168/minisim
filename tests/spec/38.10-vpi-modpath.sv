// IEEE 1800-2023 37 (module path objects) and 38.10: vpi_iterate(vpiModPath,
// module) gives the paths of a module instance (one per input / output
// pair), vpiModPathIn / vpiModPathOut their terms (vpiEdge, vpiExpr: the
// signal), vpiModPathHasIfNone; vpi_get_delays reads a path's delays and
// vpi_put_delays changes them: y then follows a with rise 5, fall 6.
//! icarus-skip: Icarus 13 has no module path delays through VPI
//! args: data/vpi/modpath.c
`timescale 1ns/1ns
module bufp(input a, input clk, input d, output y, output reg q);
  assign y = a;
  always @(posedge clk) q <= d;
  specify
    (a => y) = (2, 3);
    (posedge clk => (q +: d)) = 4;
    if (d) (clk => q) = 1;
    ifnone (clk => q) = 2;
  endspecify
endmodule
module top;
  reg a = 0, clk = 0, d = 0;
  wire y, q;
  bufp b(.a(a), .clk(clk), .d(d), .y(y), .q(q));
  initial begin
    #10 a = 1;
    #10 a = 0;
    #10 $paths;
    a = 1;
    #10 a = 0;
    #10 $finish;
  end
  always @(y) $display("%0t y %b", $time, y);
endmodule
//> 3 y 0
//> 12 y 1
//> 23 y 0
//> vpiModPath ifnone 0: in a edge 0 out y (top.b.y) delays 2 3 2
//> vpiModPath ifnone 0: in clk edge 13 out q (top.b.q) delays 4 4 4
//> vpiModPath ifnone 0: in clk edge 0 out q (top.b.q) delays 1 1 1
//> vpiModPath ifnone 1: in clk edge 0 out q (top.b.q) delays 2 2 2
//> a => y now: delays 5 6 5
//> 35 y 1
//> 46 y 0
