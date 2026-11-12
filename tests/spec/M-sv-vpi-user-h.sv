// IEEE 1800-2023 annex M: sv_vpi_user.h. Every object type, relation,
// property, operator, callback reason and control constant of the
// SystemVerilog, assertion (clause 39) and coverage (clause 40) parts of
// VPI has the number the standard gives it, and the assertion callback
// structures and vpi_register_assertion_cb are declared.
//! icarus-skip: Icarus 13 has no sv_vpi_user.h
//! args: data/vpi/m_constants.c
module top;
  initial $check_m;
endmodule
//> sv_vpi_user.h: 399 constants, 0 wrong
