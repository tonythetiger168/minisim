// Nothing to report: every path assigns, the right assignment kind in each
// block, one driver per value, and a two-flop synchronizer for the crossing.
// Also: two drivers in opposite branches of a generate if (only one is
// elaborated), separate bits driven by a generate loop, and a clock passed
// down a port to a submodule (one domain, not two).
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
  localparam bit Fast = 1;
  logic [3:0] g, bits;
  if (Fast) begin : g_fast
    assign g = a;
  end else begin : g_slow
    assign g = ~a;
  end
  for (genvar i = 0; i < 4; i++) begin : g_bits
    assign bits[i] = a[i] ^ g[i];
  end
  logic sub_q;
  clean_sub u_sub(.clk(bclk), .d(s2), .q(sub_q));
endmodule

// Its flop is in bclk's domain: reading s2 (bclk) is not a crossing.
module clean_sub(input logic clk, d, output logic q);
  logic q1;
  always_ff @(posedge clk) q1 <= d;
  assign q = q1;
endmodule
