// IEEE 1800-2023 38.10: vpi_get_delays reads the rise, fall and turn-off
// delays of a continuous assignment (10.3.3) and vpi_put_delays changes
// them: y then follows a 4 later. An assignment without a delay has none
// to read (an error).
//! icarus-skip: Icarus 13 has no continuous assignment delays through VPI
//! args: data/vpi/assigndly.c
`timescale 1ns/1ns
module top;
  reg a = 0;
  wire y, z;
  assign #(2, 3) y = a;
  assign z = a;
  initial begin
    #10 a = 1;
    #10 a = 0;
    #10 $assigns;
    a = 1;
    #10 a = 0;
    #10 $finish;
  end
  always @(y) $display("%0t y %b", $time, y);
endmodule
//> 3 y 0
//> 12 y 1
//> 23 y 0
//> assign y: delays 2 3 2
//> assign z: no delay, error 1
//> now: delays 4 4 4
//> 34 y 1
//> 44 y 0
