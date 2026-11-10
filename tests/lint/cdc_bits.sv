// clk[0] and clk[1] are two clocks, and a Gray-coded bus synchronized one bit
// at a time through a synchronizer cell is one clean crossing.
module cdc_bits_sync(input logic clk, input logic d, output logic q);
  logic [1:0] r;
  always_ff @(posedge clk) r <= {r[0], d};
  assign q = r[1];
endmodule
module cdc_bits_src(input logic clk, input logic [3:0] n, output logic [3:0] o);
  logic [3:0] w;
  always_ff @(posedge clk) w <= n ^ (n >> 1);
  assign o = w;
endmodule
module cdc_bits(input logic [1:0] clk, input logic [3:0] n, output logic [3:0] y);
  logic [3:0] x;
  cdc_bits_src u(.clk(clk[0]), .n, .o(x));
  for (genvar i = 0; i < 4; i++) begin : g
    cdc_bits_sync i_s(.clk(clk[1]), .d(x[i]), .q(y[i]));
  end
endmodule
