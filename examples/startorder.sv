// Time 0 start order (msim.cpp, codegen.hpp): declaration order, except
// that a constant continuous assign before the first initial block starts
// just before it. So the always blocks that read `in` see its change
// (Icarus prints "6 5" too), and `initial clk = 1` before the always
// @(posedge clk) is no edge for it (n=1).
module startorder;
  wire [7:0] in;
  reg [7:0] o1, o2;
  assign in = 8'd5;
  always @* o1 = in + 1;
  always @(in) o2 = in;
  reg clk;
  integer n = 0;
  initial clk = 1;
  always @(posedge clk) n = n + 1;
  initial begin #1 clk = 0; #1 clk = 1; #1 $display("%0d %0d n=%0d", o1, o2, n); end
endmodule
