// VCS VPD tasks on msim: the waveform is written as FST (vendor_tasks.hpp).
module sub(input logic clk, output logic [3:0] q);
  always @(posedge clk) q <= q + 1;
  initial q = 0;
endmodule
module tb;
  logic clk = 0; logic [3:0] q; logic [7:0] mem [0:3];
  sub u1(.clk, .q);
  always #5 clk = ~clk;
  initial begin
    $vcdplusfile("run.vpd");
    $vcdpluson(0, tb.u1);
    $vcdplusmemon(mem);
    #20 $vcdplusoff;
    #20 $vcdpluson;
    #10 $vcdplusflush; $vcdplusclose; $finish;
  end
endmodule
