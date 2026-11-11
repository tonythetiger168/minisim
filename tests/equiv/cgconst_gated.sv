// cgconst.sv with clock gates whose enables fold to constant 1 and constant 0.
module icg (input wire clk, input wire en, output wire gclk);
  reg en_l;
  always @(*) if (!clk) en_l = en;
  assign gclk = clk & en_l;
endmodule
module t (input clk, input en, input [7:0] d, output reg [7:0] q, output reg [7:0] p);
  wire g1, g0;
  icg u1 (.clk(clk), .en(en || !en), .gclk(g1));
  icg u0 (.clk(clk), .en(en && !en), .gclk(g0));
  always @(posedge g1) if (en || !en) q <= d;
  always @(posedge g0) if (en && !en) p <= d;
endmodule
