// IEEE 1800-2023 Annex D: optional system tasks and functions.
// D.2 $countdrivers counts the drivers of a scalar net (1 when more than
// one drives it) and returns the counts in its output arguments; D.3
// $getpattern passes a memory word through; D.14 $sreadmemh / $sreadmemb
// load a memory from strings in the $readmem file format; D.10 $scale gives
// one time unit of another module in this module's units; D.11 $scope sets
// the interactive scope (the first top-level module until then) that D.12
// $showscopes, D.13 $showvars and D.6 $list read.
//! icarus-skip: Icarus has no Annex D tasks
`timescale 1us / 1ns
module sub;
  reg [3:0] r = 4'h5;
  task t; endtask
endmodule
`timescale 1ns / 1ns
module top;
  wire a;
  wire b0, b1;
  assign a = 1'b1;
  assign a = 1'b0;
  assign b0 = 1'b1;
  reg [7:0] m [0:3];
  wire [3:0] p;
  reg [1:0] idx = 0;
  assign p = $getpattern(m[idx][3:0]);
  sub u1();
  integer f, n01x, n0, n1, nx;
  initial begin : blk
    bit c;
    c = $countdrivers(a, f, n01x, n0, n1, nx);
    $display("a: %0d forced=%0d 01x=%0d 0=%0d 1=%0d x=%0d", c, f, n01x, n0, n1, nx);
    c = $countdrivers(b0, f, n01x);
    $display("b0: %0d %0d", c, n01x);
    c = $countdrivers(b1, f, n01x);
    $display("b1: %0d %0d", c, n01x);
    $sreadmemh(m, 0, 3, "0a 0b", "@3 0d");
    $sreadmemb(m, 2, 2, "1100");
    $display("%h %h %h %h", m[0], m[1], m[2], m[3]);
    #1 $display("p=%h", p);
    idx = 3;
    #1 $display("p=%h", p);
    $display("scale=%0d", $scale(u1));
    $showscopes;
    $scope(top.u1);
    $showvars;
    $showscopes;
    $scope(top);
    $showvars(idx);
    $list(top.u1.t);
  end
endmodule
//> a: 1 forced=0 01x=2 0=1 1=1 x=0
//> b0: 0 1
//> b1: 0 0
//> 0a 0b 0c 0d
//> p=a
//> p=d
//> scale=1000
//> module top.u1 (sub)
//> named block top.blk
//> top.u1.r (reg[3:0]) = 0101
//> task top.u1.t
//> top.idx (reg[1:0]) = 11
//> task t; endtask
