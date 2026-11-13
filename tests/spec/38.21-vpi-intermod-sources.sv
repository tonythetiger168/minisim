// IEEE 1800-2023 38.21: on a net with several sources, each inter-module
// path from a source port to a load port has its own delay. Here u1 and u2
// both drive n, which u3 reads: the path u1.o -> u3.i gets 3 and
// u2.o -> u3.i gets 7, so the load sees a change from u1 after 3 and one
// from u2 after 7. vpi_get_delays reads each path's own delay back.
//! icarus-skip: Icarus 13 has no vpi_handle_multi
//! args: data/vpi/intermodsrc.c
`timescale 1ns/1ns
module drv(input logic a, en, output wire o);
  assign o = en ? a : 1'bz;
endmodule
module rcv(input wire i, output wire y);
  assign y = i;
endmodule
module top;
  logic a1 = 0, a2 = 0, en1 = 1, en2 = 0;
  wire n, y;
  drv u1(.a(a1), .en(en1), .o(n));
  drv u2(.a(a2), .en(en2), .o(n));
  rcv u3(.i(n), .y(y));
  initial begin
    #1 $intermod_src;
    #9 a1 = 1;                    // through u1: y at 13
    #10 en1 = 0; en2 = 1;         // u1 lets go at 20 (seen at 23), u2 drives 0 (seen at 27)
    #20 a2 = 1;                   // through u2: y at 47
    #10 $finish;
  end
  always @(y) $display("%0t y %b", $time, y);
endmodule
//> 0 y 0
//> u1 path delays 3 3 3
//> u2 path delays 7 7 7
//> 13 y 1
//> 23 y z
//> 27 y 0
//> 47 y 1
