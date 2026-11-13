// Multi-step front end test (tests/msim_mstep.cmake): a header from
// +incdir+, a macro from +define+, a package from another library, a VHDL
// entity analysed with vcom / vhdlan, a plusarg at run time.
`include "w.svh"
module tb;
  import p::*;
  logic clk = 0, rst = 1, en = 0;
  wire [7:0] count;
  counter #(.WIDTH(8)) dut (.clk(clk), .rst(rst), .en(en), .count(count));
  always #5 clk = ~clk;
  initial begin
    #12 rst = 0; en = 1;
    repeat (K) @(posedge clk);
    #1 $display("%s W=%0d count=%0d", `GREETING, `W, count);
    if ($test$plusargs("more")) $display("more");
    $finish;
  end
endmodule
