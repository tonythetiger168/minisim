// IEEE 1800-2023 22.7: each module's delays are in its own time unit, rounded
// to its precision. 20.3.1: $time is in the calling module's unit.
`timescale 1ns/100ps
module sub;
  initial #1.55 $display("sub %0d", $time);
endmodule
`timescale 10ns/1ns
module top;
  sub s();
  initial begin
    #1.26 $display("top %0d", $time);
    #2 $display("top %0d", $time);
  end
endmodule

//> sub 2
//> top 1
//> top 3
