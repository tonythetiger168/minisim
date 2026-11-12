// Bits of one vector and fields of one struct that are written separately
// are separate signals.
module cdc_slices(input logic aclk, bclk, d, e,
                  output logic o1, o2, o3, o4);
  typedef struct packed { logic req; logic [3:0] tag; } ctl_t;

  logic a1, a2;
  always_ff @(posedge aclk) begin a1 <= d; a2 <= e; end

  // Two synchronizers kept in one vector each: s[1:0] for a1, s[3:2] for a2.
  logic [3:0] s;
  always_ff @(posedge bclk) begin
    s[0] <= a1;
    s[1] <= s[0];
    s[2] <= a2;
    s[3] <= s[2];
  end
  // Only the second stages are used: clean.
  always_ff @(posedge bclk) o1 <= s[1];
  always_ff @(posedge bclk) o2 <= s[3];

  // A register bank: bit 0 is in domain B, bit 1 synchronizes a1 again.
  logic [2:0] r;
  always_ff @(posedge bclk) r[0] <= ~r[0];
  always_ff @(posedge bclk) r[1] <= a1;      // first stage, read below: CDC001
  always_ff @(posedge bclk) r[2] <= r[1] & r[0];
  assign o3 = r[2];

  // Struct fields: req is synchronized, tag stays in domain A.
  ctl_t c_a;
  always_ff @(posedge aclk) begin c_a.req <= d; c_a.tag <= {4{e}}; end
  logic q1, q2;
  always_ff @(posedge bclk) begin q1 <= c_a.req; q2 <= q1; end
  assign o4 = q2;
endmodule
