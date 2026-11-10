// IEEE 1800-2023 33.8.1: -L <library> sets the library search order,
// overriding the order of the library map (33.8.2's map: L1 comes first
// there, so without -L the cells come from L1).
//! icarus-skip: Icarus 13 does not support library maps
//! args: -libmap data/cfg/33.8/proj/lib.map -L L2 -L L1
module tb;
  a ua();
  b ub();
endmodule

//> tb.ua: L2.a from lib2/gates/a.v
//> tb.ub: L2.b from lib2/gates/b.v
