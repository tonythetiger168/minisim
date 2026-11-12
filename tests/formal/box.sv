// -blackbox and -cut: abstraction by option.
module mulbox(input logic clk, input logic [15:0] x, y, output logic [31:0] p);
  always_ff @(posedge clk) p <= x * y;
  a_in_box: assert property (@(posedge clk) p[31:0] >= 0);
endmodule
module box(input logic clk, input logic rst, input logic [15:0] x, y, input logic go, output logic [31:0] acc);
  logic [31:0] p;
  logic [3:0]  cnt;
  mulbox u_mul(.clk, .x, .y, .p);
  always_ff @(posedge clk)
    if (rst) begin acc <= '0; cnt <= '0; end
    else if (go) begin acc <= p; cnt <= cnt == 4'd9 ? 4'd0 : cnt + 4'd1; end
  // does not depend on the multiplier: holds with or without the box
  a_cnt: assert property (@(posedge clk) disable iff (rst) cnt <= 4'd9);
  // depends on the multiplier: true in the design, not once it is a black box
  a_zero: assert property (@(posedge clk) disable iff (rst) x == 0 |=> p == 0);
  // true only while cnt stays in range: fails when cnt is cut
  a_ten: assert property (@(posedge clk) disable iff (rst) cnt != 4'd12);
endmodule
