// IEEE 1800-2023 6.6.8: an interconnect net is typeless; it takes the type
// of the net ports it connects and passes the driven value through.
//! icarus-skip: Icarus 13 does not parse interconnect
module drv(output wire [3:0] o); assign o = 4'h9; endmodule
module rcv(input wire [3:0] i); initial #1 $display("%h", i); endmodule
module top;
  interconnect bus;
  drv d(.o(bus));
  rcv r(.i(bus));
endmodule
//> 9
