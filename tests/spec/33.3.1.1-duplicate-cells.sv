// IEEE 1800-2023 33.3.1.1: when two cells with one name map to the same
// library in one invocation, the last one encountered is written to the
// library, with a warning. Here module leaf appears twice in work (in
// this file), and adder twice in lib1 (first.v, then second.v).
//! icarus-skip: Icarus 13 does not support library maps
//! args: -libmap data/cfg/dup/lib.map data/cfg/dup/first.v data/cfg/dup/second.v
module leaf;
  initial $display("%m: the first cell");
endmodule
module leaf;
  initial $display("%m: the second cell");
endmodule
module top;
  leaf c();
  adder a();
endmodule
//> top.c: the second cell
//> top.a: adder from second.v
