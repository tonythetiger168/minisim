// IEEE 1800-2023 33.2, 33.3.1, 33.5.4: the example of 33.2. The library map
// puts top.v and adder.v in rtlLib and adder.vg in gateLib (the files are
// read through the map); the config binds top.a2 to the gate-level adder.
// With no -top, the config on the command line names the top-level module.
//! icarus-skip: Icarus 13 does not support configurations
//! args: -libmap data/cfg/33.2/lib.map

config cfg1; // specify rtl adder for top.a1, gate-level adder for top.a2
  design rtlLib.top;
  default liblist rtlLib;
  instance top.a2 liblist gateLib;
endconfig

//> top.a1: rtl adder from rtlLib.adder
//> top.a2: gate-level adder from gateLib.adder
