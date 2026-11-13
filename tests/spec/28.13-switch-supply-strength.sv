// IEEE 1800-2023 28.13: a supply-strength value is reduced to strong through
// a non-resistive MOS switch, and to pull through a resistive one.
//! icarus-skip: Icarus 13 passes supply as strong through rnmos
module top;
  supply1 vdd;
  supply0 gnd;
  wire n_out, rn_out, p_out, rp_out, c_out;
  reg on = 1, off = 0;
  nmos   n1(n_out, vdd, on);
  rnmos  n2(rn_out, vdd, on);
  pmos   p1(p_out, gnd, off);
  rpmos  p2(rp_out, gnd, off);
  rcmos  c1(c_out, vdd, on, off);
  initial #1 $display("%v %v %v %v %v", n_out, rn_out, p_out, rp_out, c_out);
endmodule
//> St1 Pu1 St0 Pu0 Pu1
