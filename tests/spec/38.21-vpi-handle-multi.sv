// IEEE 1800-2023 38.21: vpi_handle_multi(vpiInterModPath, out, in) gives
// the path between an output port of one instance and an input port of
// another on the same net (in either order, the same object); its ports
// through vpiPort; vpi_put_delays gives it an interconnect delay (rise 3,
// fall 5) that the load port then sees, and vpi_get_delays reads it back.
//! icarus-skip: Icarus 13 has no vpi_handle_multi
//! args: data/vpi/intermod.c
`timescale 1ns/1ns
module drv(input logic a, output wire o);
  assign o = a;
endmodule
module rcv(input wire i, output wire y);
  assign y = i;
endmodule
module top;
  logic a = 0;
  wire n, y;
  drv u1(.a(a), .o(n));
  rcv u2(.i(n), .y(y));
  initial begin
    #1 $intermod;
    #9 a = 1;
    #10 a = 0;
  end
  always @(n, y) $display("%0t n %b y %b", $time, n, y);
endmodule
//> 0 n 0 y z
//> 0 n 0 y 0
//> vpiInterModPath
//>   port o dir 2
//>   port i dir 1
//> same object either order: 1
//> delays 0 0 0
//> delays 3 5 3
//> error: vpi_handle_multi: the ports are not connected to one net
//> error: vpi_handle_multi: only vpiInterModPath is defined
//> 10 n 1 y 0
//> 13 n 1 y 1
//> 20 n 0 y 1
//> 25 n 0 y 0
