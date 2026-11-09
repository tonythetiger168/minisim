// 21.7.1 VCD tasks: $dumpfile, $dumpvars with levels and scope / variable
// arguments, $dumpoff, $dumpon, $dumpall, $dumpflush and $dumplimit run
// without changing the simulation. tests/msim_vcdtasks.cmake checks the file.
module sub(input logic c, output logic [3:0] q);
  logic [3:0] r = 0;
  always @(posedge c) r <= r + 1;
  assign q = r;
endmodule
module tb;
  logic clk = 0;
  logic [3:0] q;
  logic [7:0] other = 8'h5a;
  sub u1(.c(clk), .q(q));
  always #5 clk = ~clk;
  initial begin
    $dumpfile("spec_21_7.vcd");
    $dumpvars(1, tb.u1);
    $dumpvars(0, other);
    $dumplimit(1000000);
    #20 $dumpoff;
    $display("off at %0t q=%0d", $time, q);
    #20 $dumpon;
    $display("on at %0t q=%0d", $time, q);
    #10 $dumpall;
    $dumpflush;
    #10 $display("end at %0t q=%0d", $time, q);
    $finish;
  end
endmodule
//> off at 20 q=2
//> on at 40 q=4
//> end at 60 q=6
