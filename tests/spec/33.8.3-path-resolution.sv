// IEEE 1800-2023 33.3.1.1, 33.8.3: when a file matches several library
// path specifications, one ending in an explicit file name wins over one
// ending in a wildcarded name, which wins over a directory.
//! icarus-skip: Icarus 13 does not support library maps
//! args: -libmap data/cfg/33.8/proj/tb/lib.map
module tb;
  foobar u1();   // lib1 ("foo*.v" beats "../lib/")
  foo u2();      // lib2 ("foo.v" beats "foo*.v" and "../lib/")
  bar u3();      // lib3 (only "../lib/")
  barver u4();   // lib4 ("*ver.v" beats "../lib/")
  initial $display("%m: %L");   // tb.sv is in no library: work
endmodule

//> tb.u1: lib1.foobar
//> tb.u2: lib2.foo
//> tb.u3: lib3.bar
//> tb.u4: lib4.barver
//> tb: work.tb
