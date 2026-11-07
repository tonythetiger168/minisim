// Testbench in a second file. Build with:
//   msim examples/cli/adder.sv examples/cli/adder_tb.sv -top adder_tb +define+WIDTH=8 -vcd adder.vcd
`ifndef WIDTH
  `define WIDTH 4
`endif
module adder_tb;
  logic clk = 0;
  logic [`WIDTH-1:0] a, b, sum;
  adder #(.W(`WIDTH)) u (.clk(clk), .a(a), .b(b), .sum(sum));
  always #5 clk = ~clk;
  initial begin
    a = 200; b = 100;
    @(posedge clk); #1;
    $display("W=%d sum=%d", `WIDTH, sum);
    $finish;
  end
endmodule
