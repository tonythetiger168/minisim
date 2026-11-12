// IEEE 1800-2023 annex L and 36.12: vpi_compatibility.h. An application
// that defines VPI_COMPATIBILITY_VERSION_1364v2005 before it includes
// vpi_user.h is bound to the 1364-2005 variants of the routines (36.12.2),
// and one that defines none runs in the simulator's default mode. Both can
// be loaded together; vpi_get(vpiCompatibilityMode, NULL) tells each one
// its own mode.
//! icarus-skip: Icarus 13 has no vpi_compatibility.h
//! args: data/vpi/compat_new.c data/vpi/compat_old.c
module top;
  reg [3:0] r = 4'd9;
  initial begin
    $mode_new;
    $mode_old(r);
    $mode_new;
  end
endmodule
//> default mode 5 (vpiMode1800v2009)
//> old application: mode 3 (vpiMode1364v2005), bound to vpi_get_1364v2005
//> old application: r is a vpiReg of size 4 = 9
//> default mode 5 (vpiMode1800v2009)
