// Nothing to report: every path assigns, the right assignment kind in each
// block, one driver per value, and a two-flop synchronizer for the crossing.
module clean(input logic aclk, bclk, rst, input logic [3:0] a, input logic req,
             output logic [3:0] y, output logic ack);
  logic [3:0] r;
  logic rq, s1, s2;
  always_comb begin
    if (a[0]) y = a;
    else      y = 4'b0;
  end
  always_ff @(posedge aclk or negedge rst) begin
    if (!rst) r <= '0;
    else      r <= a;
  end
  always_ff @(posedge aclk) rq <= req;
  always_ff @(posedge bclk) s1 <= rq;
  always_ff @(posedge bclk) s2 <= s1;
  always_comb ack = s2 & (r != 0);
endmodule
