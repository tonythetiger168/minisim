// IEEE 1800-2023 33.6.2: a default liblist sets the library search order for every
// instance. cfg2 puts gateLib before aLib and rtlLib, so adder
// and m are the gate-level ones and top is still the rtl one.
//! icarus-skip: Icarus 13 does not support configurations
//! args: -libmap data/cfg/33.6/lib.map

config cfg2;
  design rtlLib.top ;
  default liblist gateLib aLib rtlLib;
endconfig

//> top.a1.f1: gateLib.m (adder.vg)
//> top.a1.f2: gateLib.m (adder.vg)
//> top.a1: gateLib.adder (adder.vg)
//> top.a2.f1: gateLib.m (adder.vg)
//> top.a2.f2: gateLib.m (adder.vg)
//> top.a2: gateLib.adder (adder.vg)
