// A 1ps time precision (the usual testbench `timescale). Delays in ns with
// fractions, $time / $realtime in the module's unit, %t with and without
// $timeformat, and a clocked design under it. msim -compile must print what
// the interpreter prints and write the same waveform.
`timescale 1ns/1ps
module tp_dut(input logic clk, input logic [7:0] d, output logic [7:0] q);
  always_ff @(posedge clk) q <= q + d;
  initial q = 0;
endmodule

`timescale 100ps/1ps
module tp_sub;
  initial begin
    #2.5;                              // 250 ps
    $display("sub: time=%0t t=%0d rt=%0.2f", $time, $time, $realtime);
  end
endmodule

`timescale 1ns/1ps
module timeps;
  logic clk = 0;
  logic [7:0] d = 1, q;
  always #2.5 clk = ~clk;              // 5 ns period
  tp_dut u(.clk(clk), .d(d), .q(q));
  tp_sub s();
  localparam string tag = "edge";      // string and real constants in compiled code
  real scale = 0.5;
  always @(posedge clk) if ($time > 100) $display("%s %0d %0.2f", tag, q, scale * q + 1.25);
  always @(negedge clk) d <= d * 8'd3 + 8'd1;
  initial begin
    #1.234;
    $display("a: %0t %0d %0.3f", $time, $time, $realtime);
    #0.0005;                           // below the precision: rounds to 1 ps
    $display("b: %t", $realtime);
    $timeformat(-9, 3, " ns", 12);
    #10.1;
    $display("c: %t q=%h", $realtime, q);
    #100;
    $display("d: %t q=%h", $time, q);
    $finish;
  end
endmodule
