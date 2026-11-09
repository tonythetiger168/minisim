// IEEE 1800-2023 33.4.2, 33.6.5: an instance bound to a config
// (use work.cfg5:config) takes its design statement and its rules: top.a2
// is aLib.adder, its f1 is rtlLib.m and its f2 gateLib.m. cfg6 is the top
// because no other config binds it.
//! icarus-skip: Icarus 13 does not support configurations
//! args: -libmap data/cfg/33.6/lib.map

config cfg5;
  design aLib.adder;
  default liblist gateLib aLib;
  instance adder.f1 liblist rtlLib;
endconfig

config cfg6;
  design rtlLib.top;
  default liblist aLib rtlLib;
  instance top.a2 use work.cfg5:config ;
endconfig

//> top.a1.f1: aLib.m (adder.v)
//> top.a1.f2: aLib.m (adder.v)
//> top.a1: aLib.adder (adder.v)
//> top.a2.f1: rtlLib.m (top.v)
//> top.a2.f2: gateLib.m (adder.vg)
//> top.a2: aLib.adder (adder.v)
