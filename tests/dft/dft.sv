module t(input logic clk, rst_n, en, input logic [3:0] a, output logic [3:0] y, output logic div_q, neg_q, gq, lq);
  logic gclk, divclk, soft_rst, loop_a, loop_b;
  // a gated clock -> DFT001
  always_comb gclk = clk & en;
  always_ff @(posedge gclk) gq <= a[0];
  // a divided clock -> DFT006 on its use
  always_ff @(posedge clk) divclk <= ~divclk;
  always_ff @(posedge divclk) div_q <= a[1];
  // a reset built from logic -> DFT002
  always_comb soft_rst = rst_n & en;
  always_ff @(posedge clk or negedge soft_rst) begin
    if (!soft_rst) y <= '0;
    else           y <= a;
  end
  // the falling edge of the same clock -> DFT004
  always_ff @(negedge clk) neg_q <= a[2];
  // a latch -> DFT003
  always_latch if (en) lq <= a[3];
  // a combinational loop -> DFT005
  always_comb loop_a = loop_b ^ a[0];
  always_comb loop_b = loop_a & en;
endmodule
