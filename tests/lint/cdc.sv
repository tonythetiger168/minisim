module sub(input logic clk, input logic d, output logic q);
  always_ff @(posedge clk) q <= d;
endmodule

module t(input logic aclk, bclk, input logic [7:0] data, input logic req,
         output logic [7:0] out, output logic ack, output logic bad);
  logic [7:0] dreg;
  logic rq, s1, s2, b1, b2;
  // domain A
  always_ff @(posedge aclk) dreg <= data;
  always_ff @(posedge aclk) rq <= req;
  // domain B: no synchronizer -> CDC001
  always_ff @(posedge bclk) bad <= rq;
  // domain B: two flops -> clean
  always_ff @(posedge bclk) s1 <= rq;
  always_ff @(posedge bclk) s2 <= s1;
  always_ff @(posedge bclk) ack <= s2;
  // domain B: a multi-bit bus through two flops -> CDC002
  logic [7:0] m1, m2;
  always_ff @(posedge bclk) m1 <= dreg;
  always_ff @(posedge bclk) m2 <= m1;
  always_ff @(posedge bclk) out <= m2;
  // through a module boundary: still one domain, so no finding
  sub u(.clk(bclk), .d(s2), .q(b1));
  always_ff @(posedge bclk) b2 <= b1;
endmodule
