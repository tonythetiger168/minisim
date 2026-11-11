// For the configuration and waiver tests: five findings by default.
module dut(input logic clk, input logic [7:0] a, input logic spare, output logic [3:0] y);
  logic [3:0] Tmp;
  assign Tmp = a[3:0];
  assign #1 y = a;
endmodule
