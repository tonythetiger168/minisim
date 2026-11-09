// IEEE 1800-2023 35.5: Calling a C library function through a DPI-C import
// (35.5).
//! icarus-skip: Icarus 13 does not support DPI
module top;
  import "DPI-C" function int abs(input int x);
  initial $display("%0d", abs(-3));
endmodule

//> 3
