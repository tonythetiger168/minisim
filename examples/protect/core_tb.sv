// Plain-text testbench for the protected secret_core.
module core_tb;
  logic clk = 0;
  logic [7:0] d = 1;
  wire [7:0] q;
  secret_core u(.clk(clk), .d(d), .q(q));
  always #5 clk = ~clk;
  always @(negedge clk) d <= d + 2;
  initial begin
    repeat (5) @(posedge clk);
    #1 $display("q=%0d", q);
    $finish;
  end
endmodule
