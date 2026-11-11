module ctl(input logic clk, rst_n, input logic [7:0] a, b, input logic [1:0] op, input logic en,
           output logic [7:0] y, output logic zero, output logic [3:0] cnt);
  logic [7:0] r;
  always_comb begin
    case (op)
      2'd0: r = a + b;
      2'd1: r = a - b;
      2'd2: r = a & b;
      default: r = a ^ b;
    endcase
  end
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin y <= '0; cnt <= '0; end
    else if (en) begin y <= r; cnt <= cnt + 4'd1; end
  assign zero = (y == 8'd0);
endmodule
