// mformal test: what cannot be modelled is cut (free in every cycle) and
// reported; a proof that does not depend on it still holds.
module cut(input logic clk, input logic rst, input logic d, output logic q, output logic slow);
  always_ff @(posedge clk) if (rst) q <= 1'b0; else q <= d;
  always #7 slow = ~slow;                      // a delay: cut
  logic n;
  always_ff @(negedge clk) n <= d;             // another edge: cut
  a_q: assert property (@(posedge clk) disable iff (rst) d |=> q);
  a_slow: assert property (@(posedge clk) disable iff (rst) !slow);
  a_n: assert property (@(posedge clk) disable iff (rst) $past(d) |-> n);
endmodule
