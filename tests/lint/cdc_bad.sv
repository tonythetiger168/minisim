// One of each CDC finding. Domain A is aclk, domain B is bclk.
module cdc_bad(input logic aclk, bclk, rst_n, input logic [3:0] din, input logic p, q,
               output logic o_none, o_glitch, o_recon, o_fan, output logic [3:0] o_bus);
  logic a1, a2, a3, a4;
  logic [3:0] abus;
  always_ff @(posedge aclk or negedge rst_n)
    if (!rst_n) begin a1 <= 0; a2 <= 0; a3 <= 0; a4 <= 0; abus <= '0; end
    else begin a1 <= p; a2 <= q; a3 <= p ^ q; a4 <= p & q; abus <= din; end

  // CDC001: logic in domain B reads a1 with nothing in between.
  logic bcnt;
  always_ff @(posedge bclk) bcnt <= bcnt ^ a1;
  assign o_none = bcnt;

  // CDC003: a1 & a2 is decoded in front of the synchronizer.
  wire glitchy = a1 & a2;
  logic g1, g2;
  always_ff @(posedge bclk or negedge rst_n)
    if (!rst_n) begin g1 <= 0; g2 <= 0; end
    else begin g1 <= glitchy; g2 <= g1; end
  assign o_glitch = g2;

  // CDC004: a3 and a4 each have a synchronizer and meet again.
  logic [1:0] s3, s4;
  always_ff @(posedge bclk) begin
    s3 <= {s3[0], a3};
    s4 <= {s4[0], a4};
  end
  always_ff @(posedge bclk) o_recon <= s3[1] & s4[1];

  // CDC002: a binary bus through flops.
  logic [3:0] m1, m2;
  always_ff @(posedge bclk) {m2, m1} <= {m1, abus};
  assign o_bus = m2;

  // CDC005: a3 is synchronized a second time.
  logic f1, f2;
  always_ff @(posedge bclk) begin f1 <= a3; f2 <= f1; end
  assign o_fan = f2;
endmodule
