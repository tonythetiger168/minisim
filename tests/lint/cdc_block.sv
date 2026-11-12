// A block with two clocks, abstracted with mlint -cdc-abstract.
module cdc_block(input logic clk_a, clk_b, d, req, output logic q, ack, y);
  // d is read in clk_a; q is launched in clk_b.
  logic a0;
  always_ff @(posedge clk_a) a0 <= d;
  // req is synchronized into clk_b inside the block.
  logic r1, r2;
  always_ff @(posedge clk_b) begin r1 <= req; r2 <= r1; end
  always_ff @(posedge clk_b) q <= r2;
  // ack is launched in clk_a.
  always_ff @(posedge clk_a) ack <= ~a0;
  // y is combinational from inputs only.
  assign y = d & req;
endmodule
