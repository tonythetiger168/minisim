module alu(input logic clk, rst_n, input logic [7:0] a, b, input logic [1:0] op, input logic en,
           output logic [7:0] y, output logic z);
  logic [7:0] r;
  always_comb case (op)
    2'd0: r = a + b;
    2'd1: r = a - b;
    2'd2: r = a & b;
    default: r = a ^ b;
  endcase
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) y <= '0; else if (en) y <= r;
  assign z = (y == 8'd0);
endmodule
