// Plain-text testbench for the protected secret_alu.
module alu_tb;
  logic clk = 0;
  logic [7:0] a = 3, b = 5;
  wire [7:0] y;
  secret_alu u(.clk(clk), .a(a), .b(b), .y(y));
  always #5 clk = ~clk;
  initial begin
    repeat (4) @(posedge clk);
    #1 $display("y=%0d", y);
    $finish;
  end
endmodule
