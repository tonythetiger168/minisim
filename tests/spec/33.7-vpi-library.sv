// IEEE 1800-2023 33.7: the VPI properties vpiLibrary, vpiCell and
// vpiConfig of each module instance, for the configuration of 33.2: top.a2
// is the gate-level adder from gateLib, and every instance is bound by
// config work.cfg1 (this file is in no library of the map, so in work).
//! icarus-skip: Icarus 13 does not support configurations
//! args: -libmap data/cfg/33.2/lib.map data/vpi/library.c

config cfg1;
  design rtlLib.top;
  default liblist rtlLib;
  instance top.a2 liblist gateLib;
endconfig
//> top: library rtlLib, cell top, config "work.cfg1"
//> top.a1: library rtlLib, cell adder, config "work.cfg1"
//> top.a2: library gateLib, cell adder, config "work.cfg1"
//> top.a1: rtl adder from rtlLib.adder
//> top.a2: gate-level adder from gateLib.adder
