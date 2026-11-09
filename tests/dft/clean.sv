// Scan-ready: one clock from a port, a reset from a port, no latch, no
// combinational loop, and only the rising edge.
module clean(input logic clk, rst_n, input logic [3:0] a, output logic [3:0] y, output logic q);
  logic [3:0] r;
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) r <= '0;
    else        r <= a;
  end
  always_ff @(posedge clk) q <= r[0];
  // A block that reads back what it just assigned is not a loop.
  logic [3:0] sum;
  always_comb begin
    sum = 4'd0;
    sum = sum ^ r;
    y = sum ^ a;
  end
endmodule
