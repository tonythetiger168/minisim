// IEEE 1800-2023 38.10, 38.30: vpi_get_delays reads the rise, fall and
// turn-off delays of a net declared with a delay (28.16), as simulation
// ticks or scaled to the module's time unit, and with min:typ:max
// triples; vpi_put_delays changes them (append_flag adds).
//! icarus-skip: Icarus 13 has no vpi_put_delays on nets
//! args: data/vpi/delays.c
`timescale 1ns/100ps
module top;
  logic d = 0;
  wire #(2, 3) w;
  assign w = d;
  initial begin
    $delays;
    #10 d = 1;
    #5 $display("@%0d w %b", $time, w);
    $putdelays;
    #10 d = 0;
    #5 $display("@%0d w %b", $time, w);
    #1 $display("@%0d w %b", $time, w);
    $delays;
  end
endmodule
//> w: rise 2 fall 3 turn-off 2 ns
//> w: ticks 20:20:20 30 20
//> on a variable: error 3
//> @15 w 1
//> @30 w 1
//> @31 w 0
//> w: rise 3 fall 5.5 turn-off 3 ns
//> w: ticks 30:30:30 55 30
//> on a variable: error 3
