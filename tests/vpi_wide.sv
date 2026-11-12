module vpi_wide(input logic clk, input logic [63:0] din, output logic [63:0] dout);
  always @(posedge clk) dout <= din;
endmodule
