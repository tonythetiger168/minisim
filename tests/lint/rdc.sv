// Reset-domain crossings.
module rdc(input logic aclk, bclk, por_n, soft_n, input logic d,
           output logic q1, q2, q3, q4, q5);
  // RDC002: por_n goes straight to flops of both domains.
  logic a0, b0;
  always_ff @(posedge aclk or negedge por_n) if (!por_n) a0 <= 0; else a0 <= d;
  always_ff @(posedge bclk or negedge por_n) if (!por_n) b0 <= 0; else b0 <= b0 ^ 1'b1;
  assign q1 = a0;
  assign q5 = b0;

  // RDC001: a reset made by a flop of domain A resets flops of domain B.
  logic wd_rst_n;
  always_ff @(posedge aclk) wd_rst_n <= d;
  always_ff @(posedge bclk or negedge wd_rst_n) if (!wd_rst_n) q2 <= 0; else q2 <= 1'b1;

  // RDC003: soft_n clears s at any time; q3 is cleared by por_n only.
  logic s;
  always_ff @(posedge aclk or negedge soft_n) if (!soft_n) s <= 0; else s <= d;
  always_ff @(posedge aclk or negedge por_n) if (!por_n) q3 <= 0; else q3 <= s;

  // RDC004 (off by default): q4 has no reset at all.
  always_ff @(posedge aclk) q4 <= s;
endmodule
