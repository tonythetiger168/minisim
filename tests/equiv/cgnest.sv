// A clock gate on a gated clock (NEST): the two latched enables act as one.
module icg (input wire clk, input wire en, output wire gclk);
  reg en_l;
  always @(*) if (!clk) en_l = en;
  assign gclk = clk & en_l;
endmodule
module t (input clk, input core_en, input en, input [7:0] d, output reg [7:0] q);
  wire cclk;
  icg u_core (.clk(clk), .en(core_en), .gclk(cclk));
`ifdef NEST
  wire g; icg u_g (.clk(cclk), .en(en), .gclk(g));
`ifdef BUG
  always @(posedge g) q <= ~d;
`else
  always @(posedge g) if (en) q <= d;
`endif
`else
  always @(posedge cclk) if (en) q <= d;
`endif
endmodule
