// Enables that fold to a constant, and (cgconst_gated.sv) the same behind clock gates.
module t (input clk, input en, input [7:0] d, output reg [7:0] q, output reg [7:0] p);
  always @(posedge clk) if (en || !en) q <= d;
  always @(posedge clk) if (en && !en) p <= d;
endmodule
