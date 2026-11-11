// A scan-stitched design for the logic BIST test: two chains (4 and 5
// flops) on one clock, an asynchronous reset, and some logic to capture.
module sdut(input logic clk, rst_n, scan_en, input logic [1:0] scan_in, output logic [1:0] scan_out,
            input logic [3:0] a, output logic [3:0] y);
  logic [8:0] r;
  wire [8:0] d = {r[4] & r[0], r[7:4] + r[3:0] + a, r[3:0] ^ {r[4], r[7:5]} ^ {4{r[8]}}};
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) r <= '0;
    else if (scan_en) r <= {r[7:4], scan_in[1], r[2:0], scan_in[0]};
    else r <= d;
  end
  assign scan_out = {r[8], r[3]};
  assign y = r[7:4] ^ r[3:0];
endmodule
