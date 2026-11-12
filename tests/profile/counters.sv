// msim -profile: two counter instances clocked 100 times, a continuous
// sum and a clock generator. The report's activation counts are fixed.
module cnt(input logic clk, output logic [7:0] q = 0);
  always_ff @(posedge clk) q <= q + 1;
endmodule
module top;
  logic clk = 0;
  logic [7:0] a, b, s;
  cnt u0(clk, a);
  cnt u1(clk, b);
  assign s = a + b;
  always #5 clk = ~clk;
  initial begin
    #1000 $display("s=%0d", s);
    $finish;
  end
endmodule
