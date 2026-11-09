// IEEE 1800-2023 23.11: bind adds an instance into every instance of a
// module (23.11).
//! icarus-skip: Icarus 13 does not support bind
module chk(input logic a);
  initial #1 $display("%m sees %b", a);
endmodule
module dut;
  logic a = 1;
endmodule
module top;
  dut d();
  bind dut chk c(a);
endmodule

//> top.d.c sees 1
