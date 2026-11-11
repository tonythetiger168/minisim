// Testbench for the mpower tests: drives dut (gates.v or rtl.sv) for 400 ns,
// busy for the first half and idle (inputs held) for the second.
`timescale 1ns/1ps
module tb;
  reg clk = 0, a = 0, b = 0;
  wire y;
  dut dut(.clk(clk), .a(a), .b(b), .y(y));
  always #5 clk = ~clk;
  integer i;
  initial begin
    for (i = 0; i < 20; i = i + 1) begin
      @(negedge clk);
      a = i[0];
      b = i[1] ^ i[2];
    end
    #200 $finish;
  end
endmodule
