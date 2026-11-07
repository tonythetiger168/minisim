// IEEE 1800-2017 6.8 Variable declarations: a static variable's initializer
// is applied before any initial or always procedure starts.
module t;
  logic [7:0] v = 5;
  logic clk = 0;
  always #5 clk = ~clk;
  initial begin
    $display("v=%0d clk=%b", v, clk);
    @(posedge clk) $display("posedge at %0t", $time);
    $finish;
  end
endmodule
//> v=5 clk=0
//> posedge at 5
