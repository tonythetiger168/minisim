// IEEE 1800-2023 33.3.1, 33.8.2: file path specifications in a library map
// use ... (any number of directories), ? and * wildcards, ./ (the map's own
// directory) and a trailing / (every file in a directory). Without a
// config, each cell comes from the first library that has it (33.6.1);
// -L puts a library first (33.8.1).
//! icarus-skip: Icarus 13 does not support library maps
//! args: -libmap data/cfg/33.8/proj/lib.map
module tb;
  a ua();
  b ub();
  c uc();
endmodule

//> tb.ua: L1.a from lib1/rtl/a.v
//> tb.ub: L1.b from lib1/rtl/b.v
//> tb.uc: L3.c from lib3/c.v
