// IEEE 1800-2023 33.6.4: an instance clause selects one instance, and its liblist is
// inherited by everything below it: top.a1 is gate-level, top.a2 and its m
// instances are the rtl ones from aLib.
//! icarus-skip: Icarus 13 does not support configurations
//! args: -libmap data/cfg/33.6/lib.map

config cfg4;
  design rtlLib.top ;
  default liblist gateLib rtlLib;
  instance top.a2 liblist aLib;
endconfig

//> top.a1.f1: gateLib.m (adder.vg)
//> top.a1.f2: gateLib.m (adder.vg)
//> top.a1: gateLib.adder (adder.vg)
//> top.a2.f1: aLib.m (adder.v)
//> top.a2.f2: aLib.m (adder.v)
//> top.a2: aLib.adder (adder.v)
