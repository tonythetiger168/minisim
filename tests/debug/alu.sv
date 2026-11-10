// mdebug self-test design: two instances of one module, a case, a flop
// with reset and enable, and a testbench that drives them.
module alu #(parameter W = 8) (
  input  logic         clk, rst, en,
  input  logic [1:0]   op,
  input  logic [W-1:0] a, b,
  output logic [W-1:0] y
);
  logic [W-1:0] r;
  always_comb begin
    case (op)
      2'd0: r = a + b;
      2'd1: r = a - b;
      2'd2: r = a & b;
      default: r = a ^ b;
    endcase
  end
  always_ff @(posedge clk or posedge rst)
    if (rst) y <= '0;
    else if (en) y <= r;
endmodule

module tb;
  logic clk = 0, rst = 1, en = 0;
  logic [1:0] op = 0;
  logic [7:0] a = 0, b = 0, y0, y1, sum;
  alu u0 (.clk, .rst, .en, .op, .a, .b, .y(y0));
  alu u1 (.clk, .rst, .en, .op(2'd2), .a(y0), .b(8'h0f), .y(y1));
  assign sum = y0 + y1;
  always #5 clk = ~clk;
  initial begin
    #12 rst = 0;
    #10 en = 1; a = 8'd20; b = 8'd5;
    #10 op = 2'd1;
    #10 op = 2'd3;
    #10 en = 0; a = 8'd99;
    #20 $display("y0=%0d y1=%0d sum=%0d", y0, y1, sum);
    $finish;
  end
endmodule
