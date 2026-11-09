// IEEE 1800-2023 33.6.3: a cell clause binds every instance of a cell: the rtl adder
// from aLib, and the gate-level m from gateLib.
//! icarus-skip: Icarus 13 does not support configurations
//! args: -libmap data/cfg/33.6/lib.map

config cfg3;
  design rtlLib.top ;
  default liblist aLib rtlLib;
  cell m use gateLib.m ;
endconfig

//> top.a1.f1: gateLib.m (adder.vg)
//> top.a1.f2: gateLib.m (adder.vg)
//> top.a1: aLib.adder (adder.v)
//> top.a2.f1: gateLib.m (adder.vg)
//> top.a2.f2: gateLib.m (adder.vg)
//> top.a2: aLib.adder (adder.v)
