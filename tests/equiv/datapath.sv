// A reference design for mequiv; datapath_net.v is its synthesized netlist.
module sub #(parameter W=4, parameter [W-1:0] K=3)(input [W-1:0] x, input s, output [W-1:0] y);
  assign y = s ? x + K : x - K;
endmodule
module datapath(input clk, input [7:0] a, b, input [2:0] s, output [7:0] o1, o2, output reg [7:0] r, output [3:0] pc, output par, output [7:0] dv, md, output signed [7:0] sd);
  wire [7:0] w[0:3];
  genvar i;
  generate for (i=0;i<4;i=i+1) begin: g
    sub #(.W(8), .K(i*5+1)) u(.x(i==0 ? a : w[i-1]), .s(s[i%3]), .y(w[i]));
  end endgenerate
  assign o1 = w[3];
  function [3:0] popc(input [7:0] v); integer k; begin popc = 0; for (k=0;k<8;k=k+1) popc = popc + v[k]; end endfunction
  assign pc = popc(a ^ b);
  assign par = ^a;
  reg [7:0] t;
  always @* begin
    t = 8'h00;
    casez (b[3:0])
      4'b1???: t = a;
      4'b01??: t = ~a;
      4'b001?: t = {a[3:0],a[7:4]};
      default: t = a + 8'd7;
    endcase
  end
  assign o2 = t;
  always @(negedge clk) r <= (a > b) ? a - b : b - a;
  assign dv = a / (b | 8'd1);
  assign md = a % (b | 8'd1);
  assign sd = $signed(a) / $signed(b | 8'd1);
endmodule
