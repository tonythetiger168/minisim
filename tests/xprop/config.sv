// msim -xprop=<file>: X-propagation set per instance, subtree and module
// (tests/xprop/config.cfg). The same flop is instantiated five times; only
// the instances the file turns on merge an unknown enable.
//! args: -xprop=config.cfg
module ff(input logic clk, en, input logic [3:0] d, output logic [3:0] q);
  always @(posedge clk) if (en) q <= d;
endmodule
module ffb(input logic clk, en, input logic [3:0] d, output logic [3:0] q);
  always @(posedge clk) if (en) q <= d;
endmodule
module sub(input logic clk, en, input logic [3:0] d, output logic [3:0] q1, q2);
  ff  a(clk, en, d, q1);
  ffb b(clk, en, d, q2);
endmodule
module top;
  logic clk = 0, en;
  logic [3:0] d, p, q, r1, r2, s1, s2;
  ff  u0(clk, en, d, p);    // tree {top} on
  ff  u1(clk, en, d, q);    // instance {top.u1} off
  sub s(clk, en, d, r1, r2);   // tree {top.s} off; module {ffb} on wins in top.s.b
  sub t(clk, en, d, s1, s2);   // on (tree {top}); module {ffb} on
  initial begin
    en = 1; d = 4'b1010; #1 clk = 1; #1 clk = 0;
    en = 1'bx; d = 4'b0110; #1 clk = 1; #1 clk = 0;
    $display("p=%b q=%b r1=%b r2=%b s1=%b s2=%b", p, q, r1, r2, s1, s2);
    $finish;
  end
endmodule
//> p=xx10 q=1010 r1=1010 r2=xx10 s1=xx10 s2=xx10
