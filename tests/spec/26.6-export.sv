// IEEE 1800-2023 26.6: A package can export a name it imported (26.6).
//! icarus-skip: Icarus 13 does not support export
package a; int v = 3; endpackage
package b; import a::*; export a::v; endpackage
module top;
  import b::*;
  initial #1 $display("%0d", v);
endmodule

//> 3
