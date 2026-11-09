// IEEE 1800-2023 19.5.1, 19.7.1: real coverpoints: tolerance ranges (+/- and
// +%-), ranges split into type_option.real_interval intervals, [N] bins over
// intervals and values, default, ignore and illegal bins, a cross of real
// coverpoints, and real parameters as bin values.
//! icarus-skip: Icarus 13 does not support covergroups
module top;
  real a, b;
  parameter real ZSTATE = -100.0;
  parameter int XSTATE = 100;
  covergroup cg_real_value;
    cvp_a: coverpoint a {
      bins highz = {[ZSTATE+/-0.1]};   // -100.1 .. -99.9
      bins x_st = {[XSTATE+%-1.0]};    // 99.0 .. 101.0
      bins a1[] = {[0.75:0.85]};       // 1 bin
      bins a2[3] = {[0.75:0.85]};      // a2[0] covers it, a2[1], a2[2] empty
      bins a3[] = {[1.0:5.0]};         // 4 bins
      bins a5 = {[0.1:$]};
      bins a6 = default;
    }
    cvp_b: coverpoint b {
      type_option.real_interval = 0.01;
      bins xstate = {XSTATE};
      bins b1[] = {[0.75:0.85]};       // 10 bins
      bins b5[] = {[0.753+/-0.01]};    // 2 bins
      ignore_bins ig = {[0.80:0.81]};
    }
    ab: cross cvp_a, cvp_b {
      bins az_and_bx = binsof(cvp_a.highz) && binsof(cvp_b.xstate);
      ignore_bins others = !binsof(cvp_a.highz) || !binsof(cvp_b.xstate);
    }
  endgroup
  cg_real_value g = new;
  real v;
  covergroup cg_fixed;
    cp: coverpoint v {
      bins fixed[4] = {[1.0:4.0], [2.0:5.0], 1.0, 4.0, 7.0};   // 4 bins: two intervals each, the values in the last
      bins fixed5[5] = {[1.0:3.0], 7.0};                       // 3 items over 5 bins: the last two empty
      illegal_bins neg = {[$:-1.0]};
    }
  endgroup
  cg_fixed f = new;
  int cov, tot;
  initial begin
    a = -99.95; b = 100;    g.sample();   // highz, xstate: the cross bin
    a = 1.5;   b = 0.755;  g.sample();   // a3 first interval, a5; b1 first interval, b5 upper interval
    a = 5.0;   b = 0.85;   g.sample();   // a3 last interval, a5; b1 last interval
    a = 0.8;   b = 0.76;   g.sample();   // a1, a2 first bin, a5; b1 second interval
    a = -3.0;  b = 0.805;  g.sample();   // a6 (default); b ignored
    void'(g.cvp_a.get_inst_coverage(cov, tot)); $display("cvp_a %0d/%0d", cov, tot);
    void'(g.cvp_b.get_inst_coverage(cov, tot)); $display("cvp_b %0d/%0d", cov, tot);
    void'(g.ab.get_inst_coverage(cov, tot));    $display("ab %0d/%0d", cov, tot);
    $display("%0.2f", g.get_inst_coverage());
    v = 3.5; f.sample();    // fixed[1], fixed[2]
    v = 7.0; f.sample();    // fixed[3], fixed5[2]
    v = -2.5; f.sample();   // illegal
    void'(f.cp.get_inst_coverage(cov, tot)); $display("cp %0d/%0d", cov, tot);
  end
endmodule

//> cvp_a 6/9
//> cvp_b 5/12
//> ab 1/1
//> 69.44
//> Error: covergroup top.cg_fixed: illegal bin cp.neg hit (value -2.5) at time 0
//> cp 4/7
