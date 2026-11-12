// CDC004 through flops: signals synchronized separately that meet again a few
// flops later are still a cycle apart.
module cdc_seq_sync(input logic clk, d, output logic q);
  logic s;
  always_ff @(posedge clk) begin s <= d; q <= s; end
endmodule

module cdc_seq(input logic aclk, bclk, d, en, output logic o1, o2, o3, o4, o5);
  logic a1, a2, a3;
  always_ff @(posedge aclk) begin a1 <= d; a2 <= ~d; a3 <= a1; end
  logic s1, s2, s3;
  cdc_seq_sync u1(.clk(bclk), .d(a1), .q(s1));
  cdc_seq_sync u2(.clk(bclk), .d(a2), .q(s2));
  cdc_seq_sync u3(.clk(bclk), .d(a3), .q(s3));

  // One flop after the synchronizers: s1 is registered, s2 is not.
  logic p1;
  always_ff @(posedge bclk) p1 <= s1 & en;
  always_ff @(posedge bclk) o1 <= p1 & s2;

  // Reported where they meet, not again further on.
  always_ff @(posedge bclk) o2 <= o1 ^ en;

  // Two flops after on both sides.
  logic x1, x2, y1, y2;
  always_ff @(posedge bclk) begin x1 <= s1 | en; x2 <= x1 ^ en; y1 <= s3 & en; y2 <= ~y1 | en; end
  always_ff @(posedge bclk) o3 <= x2 ^ y2;

  // Three flops: past the default depth, found with -cdc-depth 3.
  logic z1, z2, z3;
  always_ff @(posedge bclk) begin z1 <= s2 ^ en; z2 <= z1 & en; z3 <= z2 | en; end
  always_ff @(posedge bclk) o4 <= z3 | s3;

  // One synchronized signal on two paths of different length is one signal.
  logic w1;
  always_ff @(posedge bclk) w1 <= s1 & en;
  always_ff @(posedge bclk) o5 <= w1 & ~s1;
endmodule
