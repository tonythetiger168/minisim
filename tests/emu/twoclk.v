// Two unrelated clocks (10 ns and 14 ns in the test) with a register file
// between them, written on one and read on the other, and a toggle that
// crosses through a two-flop synchronizer.
module twoclk (input clka, input clkb, input rst_n, input [7:0] din, input [3:0] wa, input [3:0] ra, input we,
               output [7:0] ca, output [7:0] cb, output [7:0] rd, output tg);
  reg [7:0] ca_r, cb_r, rd_r;
  reg tog, s1, s2;
  wire [7:0] rf_q;
  RF16X8 u_rf (.WCLK(clka), .WE(we), .WA(wa), .D(din ^ ca_r), .RA(ra), .Q(rf_q));
  always @(posedge clka or negedge rst_n)
    if (!rst_n) begin ca_r <= 8'd0; tog <= 1'b0; end
    else begin ca_r <= ca_r + din; tog <= tog ^ din[0]; end
  always @(posedge clkb or negedge rst_n)
    if (!rst_n) begin cb_r <= 8'd0; s1 <= 1'b0; s2 <= 1'b0; rd_r <= 8'd0; end
    else begin s1 <= tog; s2 <= s1; cb_r <= cb_r + {7'd0, s2}; rd_r <= rf_q; end
  assign ca = ca_r;
  assign cb = cb_r;
  assign rd = rd_r;
  assign tg = s2;
endmodule
