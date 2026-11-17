module tb;
  logic clk = 0;
  alu_pkg::op_t op;
  logic [7:0] a, b, y;
  alu #(.W(8)) dut (.clk(clk), .op(op), .a(a), .b(b), .y(y));
  always #5 clk = ~clk;
  initial begin
    op = alu_pkg::ADD; a = 3; b = 4;
    @(posedge clk); @(posedge clk);
    $display("y=%0d", y);
    $finish;
  end
endmodule
