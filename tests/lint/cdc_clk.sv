// How clocks are used and made.
module cdc_clk(input logic aclk, bclk, sel, en, d, output logic q1, q2, q3, q4);
  always_ff @(posedge bclk) q4 <= d;  // bclk is a clock
  // CDC007: bclk sampled as data in aclk.
  always_ff @(posedge aclk) q1 <= bclk;
  // CDC008: a clock mux made of gates.
  logic mclk;
  assign mclk = sel ? bclk : aclk;
  always_ff @(posedge mclk) q2 <= d;
  // A clock gate: not reported.
  logic en_l, gclk;
  always_latch if (!aclk) en_l = en;
  assign gclk = aclk & en_l;
  always_ff @(posedge gclk) q3 <= d;
endmodule
