// A design with the clocking an emulator has to convert: a gated clock, a
// divided clock, a falling-edge register, an asynchronous reset, a memory
// written on the divided clock, and a memory read without a clock.
module clocks (input clk, input rst_n, input en, input [7:0] d, input [3:0] wa, input [3:0] ra, input we,
               output [7:0] q, output [7:0] sr, output [7:0] nq, output [7:0] m, output [7:0] rf, output dv);
  reg div;
  wire gclk = clk & en;
  always @(posedge clk or negedge rst_n) if (!rst_n) div <= 1'b0; else div <= ~div;
  reg [7:0] q_r, sr_r, nq_r;
  always @(posedge gclk) q_r <= d + 8'd3;
  always @(posedge div or negedge rst_n) if (!rst_n) sr_r <= 8'd0; else sr_r <= {sr_r[6:0], d[0] ^ q_r[1]};
  always @(negedge clk) nq_r <= q_r ^ sr_r;
  SRAM16X8 u_ram (.CLK(clk), .CS(1'b1), .WE(we), .A(we ? wa : ra), .D(d), .Q(m));
  RF16X8 u_rf (.WCLK(div), .WE(we), .WA(wa), .D(q_r), .RA(ra), .Q(rf));
  assign q = q_r;
  assign sr = sr_r;
  assign nq = nq_r;
  assign dv = div;
endmodule
