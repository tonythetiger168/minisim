module t(input logic clk, rst, input logic [3:0] a, output logic [3:0] y, z);
  logic [3:0] r;
  always_comb begin
    if (a[0]) y = a;         // L001: no else
  end
  always_comb z <= a;        // L003
  always_ff @(posedge clk) begin
    if (rst) r = 0;          // L002
    else r <= a;
  end
  assign z = a;              // L004 with the always_comb above
endmodule
