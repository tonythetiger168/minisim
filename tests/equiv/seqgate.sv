// Sequential clock gating: stage 2 only needs to load when stage 1 changed.
// The gated form keeps a flop that remembers whether it did; the two are the
// same only in states where that flop is right, which is every state after reset.
module seqgate(input logic clk, rst_n, in_valid, input logic [7:0] a, b,
               output logic [15:0] prod);
  logic [7:0] a_q, b_q;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin a_q <= '0; b_q <= '0; end
    else if (in_valid) begin a_q <= a; b_q <= b; end
`ifdef GATED
  logic moved;  // stage 1 loaded in the last cycle
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) moved <= 1'b1;
`ifdef BUG_STUCK
    else moved <= 1'b0;
`else
    else moved <= in_valid;
`endif
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) prod <= '0;
`ifdef BUG_EARLY
    else if (in_valid) prod <= a_q * b_q;
`else
    else if (moved) prod <= a_q * b_q;
`endif
`else
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) prod <= '0;
    else prod <= a_q * b_q;
`endif
endmodule
