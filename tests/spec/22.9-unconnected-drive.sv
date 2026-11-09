// IEEE 1800-2023 22.9 `unconnected_drive pull1 makes unconnected input
// ports of the modules that follow pulled up; `nounconnected_drive ends it,
// and an unconnected input port is then left floating (z).
`unconnected_drive pull1
module sub(input a, input [1:0] b, output y, output [1:0] z);
  assign y = a; assign z = b;
endmodule
`nounconnected_drive
`unconnected_drive pull0
module subp0(input a, output y); assign y = a; endmodule
`nounconnected_drive
module sub0(input a, output y); assign y = a; endmodule
module top;
  wire y, yp0, y0; wire [1:0] z;
  sub s(.y(y), .z(z));
  subp0 sp0(.y(yp0));
  sub0 s0(.y(y0));
  initial #1 $display("%b %b %b %b", y, z, yp0, y0);
endmodule
//> 1 11 0 z
