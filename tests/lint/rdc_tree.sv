// How resets are made: reset synchronizers and logic on a reset.
module rdc_tree(input logic clk, por_n, ext_rst_n, scan_mode, scan_rst_n, d,
                output logic q1, q2, q3, q4, q5);
  // A proper reset synchronizer: asserted at once, released on the clock.
  logic r1, rst_n;
  always_ff @(posedge clk or negedge por_n)
    if (!por_n) {rst_n, r1} <= '0; else {rst_n, r1} <= {r1, 1'b1};
  // A scan mux on it is not logic that glitches.
  logic rst_scan_n;
  assign rst_scan_n = scan_mode ? scan_rst_n : rst_n;
  always_ff @(posedge clk or negedge rst_scan_n) if (!rst_scan_n) q1 <= 0; else q1 <= d;

  // RDC005: one flop only.
  logic one_n;
  always_ff @(posedge clk or negedge por_n) if (!por_n) one_n <= 0; else one_n <= 1'b1;
  always_ff @(posedge clk or negedge one_n) if (!one_n) q2 <= 0; else q2 <= d;

  // RDC005: an input reset sampled once.
  logic ext_q;
  always_ff @(posedge clk) ext_q <= ext_rst_n;
  always_ff @(posedge clk or negedge ext_q) if (!ext_q) q3 <= 0; else q3 <= d;

  // Sampled twice: fine.
  logic e1, e2;
  always_ff @(posedge clk) begin e1 <= ext_rst_n; e2 <= e1; end
  always_ff @(posedge clk or negedge e2) if (!e2) q4 <= 0; else q4 <= d;

  // RDC006: two flops decoded into a reset.
  logic wd, sw, both_n;
  always_ff @(posedge clk) begin wd <= d; sw <= ~d; end
  assign both_n = ~(wd & sw);
  always_ff @(posedge clk or negedge both_n) if (!both_n) q5 <= 0; else q5 <= d;
endmodule
