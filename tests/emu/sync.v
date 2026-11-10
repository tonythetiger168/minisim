// One clock, rising edges only: the case that runs at one emu_clk cycle per
// clock cycle. A small accumulator with a memory.
module sync (input clk, input rst, input [7:0] a, input [7:0] b, input [3:0] addr, input we,
             output [7:0] sum, output [7:0] mq, output [15:0] acc, output par);
  reg [7:0] a_r, b_r, sum_r;
  reg [15:0] acc_r;
  reg par_r;
  wire [7:0] mem_q;
  SRAM16X8 u_ram (.CLK(clk), .CS(1'b1), .WE(we), .A(addr), .D(sum_r), .Q(mem_q));
  always @(posedge clk) begin
    a_r <= a;
    b_r <= b;
    if (rst) begin
      sum_r <= 8'd0;
      acc_r <= 16'd0;
      par_r <= 1'b0;
    end else begin
      sum_r <= a_r + b_r;
      acc_r <= acc_r + {8'd0, sum_r} + {8'd0, mem_q};
      par_r <= ^{a_r, b_r, acc_r};
    end
  end
  assign sum = sum_r;
  assign mq = mem_q;
  assign acc = acc_r;
  assign par = par_r;
endmodule
