// mdebug -lint: mlint findings in the page (tests/mdebug_lint.cmake).
module lintme(input logic clk, input logic [3:0] a, output logic [3:0] y);
  logic [3:0] unused_r;
  logic [7:0] wide;
  logic q;
  always @(posedge clk) q = a[0];
  assign wide = a;
  assign y = wide;
endmodule
