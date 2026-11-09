// IEEE 1800-2023 33.6.1: with no configuration, each cell comes from the first library in the
// library map that has it: adder from aLib (adder.v), m from rtlLib (top.v).
// Nothing instantiates top, so it is the top-level module (23.3.1).
//! icarus-skip: Icarus 13 does not support configurations
//! args: -libmap data/cfg/33.6/lib.map

//> top.a1.f1: rtlLib.m (top.v)
//> top.a1.f2: rtlLib.m (top.v)
//> top.a1: aLib.adder (adder.v)
//> top.a2.f1: rtlLib.m (top.v)
//> top.a2.f2: rtlLib.m (top.v)
//> top.a2: aLib.adder (adder.v)
