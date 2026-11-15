// Instances whose outputs go to elements of a top-level array: the units
// that write sum[i] move by another distance than the rest of the
// instance, and must not keep the rest from sharing one copy of the code.
module acc(input logic clk, input logic rst, input logic [7:0] k, output logic [31:0] out);
  logic [31:0] s, t, u;
  assign t = s ^ {24'b0, k};
  assign u = t + (s >> 3);
  always_comb out = u - 32'd1;
  always_ff @(posedge clk) s <= rst ? 32'd0 : u + {24'b0, k};
endmodule
module shareport;
  logic clk = 0, rst = 1;
  logic [31:0] sum [4];
  for (genvar i = 0; i < 4; i++) begin : g
    acc a(.clk(clk), .rst(rst), .k(8'(i * 3 + 1)), .out(sum[i]));
  end
  always #5 clk = ~clk;
  initial begin
    #12 rst = 0;
    #200;
    for (int i = 0; i < 4; i++) $display("sum[%0d]=%h", i, sum[i]);
    $finish;
  end
endmodule
