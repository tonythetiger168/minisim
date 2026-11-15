// IEEE 1800-2023 10.3.3: a rise / fall delay on a continuous assignment to
// one bit of a vector picks the delay from that bit's change.
`timescale 1ns/1ns
module t;
  reg i;
  wire [3:0] o;
  assign #(1, 3) o[2] = i;
  initial begin
    i = 1;
    #5 i = 0;
    #2 $display("%b", o[2]);
    #1 $display("%b", o[2]);
  end
endmodule
//> 1
//> 0
