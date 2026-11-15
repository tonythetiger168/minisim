// Time 0 start order (msim.cpp, codegen.hpp): a constant continuous
// assign starts after the always blocks that read it, so they see its
// change, and initial blocks start last (Icarus prints "6 5" too).
module startorder;
  wire [7:0] in;
  reg [7:0] o1, o2;
  assign in = 8'd5;
  always @* o1 = in + 1;
  always @(in) o2 = in;
  initial #1 $display("%0d %0d", o1, o2);
endmodule
