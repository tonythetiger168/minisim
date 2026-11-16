// A generate-loop chain through one vector: t[(i+1)*8 +: 8] depends on
// t[i*8 +: 8]. Signal by signal that is a loop; bit by bit it is not.
module partchain;
  reg clk = 0;
  reg [7:0] d [0:7];
  reg [7:0] sel = 1;
  wire [9*8-1:0] t;
  wire [7:0] q;
  assign t[7:0] = 8'h0;
  genvar i;
  generate for (i = 0; i < 8; i = i + 1) begin : g
    assign t[(i+1)*8 +: 8] = ({8{sel[i]}} & d[i]) | t[i*8 +: 8];
  end endgenerate
  assign q = t[8*8 +: 8];
  reg [31:0] acc = 0;
  integer k;
  initial for (k = 0; k < 8; k = k + 1) d[k] = 8'h11 * k + 3;
  always #1 clk = ~clk;
  always @(posedge clk) begin
    sel <= {sel[6:0], sel[7]} ^ acc[7:0];
    d[acc[2:0]] <= d[acc[2:0]] + q;
    acc <= acc * 33 + q;
  end
  initial begin #400; $display("acc=%h q=%h", acc, q); $finish; end
endmodule
