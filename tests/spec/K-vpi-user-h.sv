// IEEE 1800-2023 annex K: vpi_user.h. Every constant the header defines has
// the number the standard gives it (an application built with another
// simulator's header passes the same numbers), for example vpiFuncType is
// 44 and vpiNetDeclAssign is 43.
//! icarus-skip: checks minisim's own vpi_user.h
//! args: data/vpi/k_constants.c
module top;
  initial $check_k;
endmodule
//> vpi_user.h: 456 constants, 0 wrong
