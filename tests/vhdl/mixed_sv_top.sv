// SystemVerilog testbench instantiating a VHDL entity (counter.vhd)
module mixed_sv_top;
  logic clk = 0, rst = 1, en = 0;
  wire [7:0] count;
  counter #(.WIDTH(8)) dut (.clk(clk), .rst(rst), .en(en), .count(count));
  always #5 clk = ~clk;
  initial begin
    #12 rst = 0; en = 1;
    repeat (10) @(posedge clk);
    #1 $display("count=%0d", count);
    $finish;
  end
endmodule
