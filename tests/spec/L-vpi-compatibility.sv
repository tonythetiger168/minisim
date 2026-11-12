// IEEE 1800-2023 annex L and 36.12: vpi_compatibility.h. An application
// that defines VPI_COMPATIBILITY_VERSION_1364v2001 before it includes
// vpi_user.h is bound to the 1364-2001 variants of the routines (36.12.2)
// and sees the object model of that standard (a bit vector is a vpiReg, a
// one-dimensional array of reg a vpiMemory),
// and one that defines none runs in the simulator's default mode. Both can
// be loaded together; vpi_get(vpiCompatibilityMode, NULL) tells each one
// its own mode.
//! icarus-skip: Icarus 13 has no vpi_compatibility.h
//! args: data/vpi/compat_new.c data/vpi/compat_old.c
module top;
  reg [3:0] r = 4'd9;
  bit [3:0] b = 4'd3;
  reg [7:0] mem [0:3];
  initial begin
    $mode_new(b, mem);
    $mode_old(r, b, mem);
    $mode_new(b, mem);
  end
endmodule
//> default mode 5 (vpiMode1800v2009): b is a vpiBitVar, mem a vpiRegArray
//> old application: mode 2 (vpiMode1364v2001), bound to vpi_get_1364v2001
//> old application: r is a vpiReg of size 4 = 9
//> old application: b is a vpiReg, mem a vpiMemory
//> default mode 5 (vpiMode1800v2009): b is a vpiBitVar, mem a vpiRegArray
