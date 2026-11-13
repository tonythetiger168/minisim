// A process of a power domain that is switched off in the middle of a body
// with several waits starts over from its top at power-up (IEEE 1801-2024
// 9.5.3): it does not resume after the wait it was in. Run with restart.upf.
module seq3(input logic clk, output logic [1:0] ph);
  always begin
    @(posedge clk) ph = 1;
    @(posedge clk) ph = 2;
    @(posedge clk) ph = 3;
  end
endmodule
module tb;
  import UPF::*;
  logic clk = 0, en = 0;
  logic [1:0] ph;
  seq3 u_s(.clk, .ph);
  always #5 clk = ~clk;
  always @(negedge clk) $display("%0t ph=%0d", $time, ph);
  initial begin
    supply_on("VDD", 1.0); supply_on("VSS", 0.0);
    en = 1;
    #20 en = 0;   // off after ph=2, waiting for the third edge
    #20 en = 1;   // on again: the next edge gives ph=1
    #50 $finish;
  end
endmodule
