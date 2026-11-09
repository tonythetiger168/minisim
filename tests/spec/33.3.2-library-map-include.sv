// IEEE 1800-2023 33.3.1, 33.3.2: a library map can include another one,
// whose relative paths are relative to the included file. A library's
// -incdir directories are searched for `include files of its sources.
//! icarus-skip: Icarus 13 does not support library maps
//! args: -libmap data/cfg/33.3.2/lib.map
module tb;
  nand2 u();
endmodule

//> tb.u: vendorLib.nand2, from the -incdir of vendorLib
