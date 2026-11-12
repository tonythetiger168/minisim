// An SoC that uses cdc_block (as RTL from cdc_block.sv, or as a black box
// from cdc_block_bb.sv) with its CDC model.
module cdc_soc(input logic fclk, sclk, x, output logic o1, o2);
  logic f_req, f_d, s_q, s_ack, y;
  always_ff @(posedge fclk) begin f_req <= x; f_d <= ~x; end
  // req crosses fclk -> sclk, synchronized in the block: clean.
  // d is read in sclk, but driven from fclk with no synchronizer: CDC001.
  cdc_block u_blk(.clk_a(sclk), .clk_b(sclk), .d(f_d), .req(f_req), .q(s_q), .ack(s_ack), .y(y));
  // q is launched in sclk and read in fclk with no synchronizer: CDC001.
  always_ff @(posedge fclk) o1 <= s_q;
  always_ff @(posedge sclk) o2 <= s_ack;
endmodule
