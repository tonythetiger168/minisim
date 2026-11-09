// IEEE 1800-2023 33.3.1.1: a file that matches path specifications of two
// libraries equally well is an error (33.8.3: /proj/lib/foover.v).
//! icarus-skip: Icarus 13 does not support library maps
//! args: -libmap data/cfg/33.8/proj/tb/conflict.map
//! exit: 1
module tb;
  foover u();
endmodule
