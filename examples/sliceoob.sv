// A constant part-select partly past the operand's width (11.5.1): the
// bits outside read x, in the compiled fast paths too. Against the interpreter.
module sliceoob;
  logic clk = 0;
  logic [3:0] a = 4'h5;
  logic [7:0] q, r;
  always #5 clk = ~clk;
  always_ff @(posedge clk) a <= a + 4'd3;
  always_comb q = a[5:2];
  always_ff @(posedge clk) r <= {a[6:3], a[1:0], 2'b01};
  initial begin
    repeat (4) @(negedge clk) $display("q=%b r=%b", q, r);
    $finish;
  end
endmodule
