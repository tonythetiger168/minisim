// IEEE 1800-2023 9.4.1: a delay control's value may be a real expression
// that changes at run time, such as #(hb) with `real hb`; it is scaled to
// the time unit and rounded to the time precision (1.4.3, 3.14.1).
`timescale 1ns/1ps
module top;
  real hb = 2.5004;     // rounds to 2.500 ns
  real r;
  logic clk = 0;
  int edges = 0;
  always #(hb) clk = ~clk;
  always @(posedge clk) edges++;
  initial begin
    #20 $display("%t edges=%0d", $realtime, edges);
    hb = 1.25;
    #20 $display("%t edges=%0d", $realtime, edges);
    r = 0.0006;         // 0.6 ps rounds to 1 ps
    #(r) $display("%t", $realtime);
    r = 3.0;
    #(r * 2) $display("%t", $realtime);
    $finish;
  end
endmodule
//>                20000 edges=4
//>                40000 edges=12
//>                40001
//>                46001
