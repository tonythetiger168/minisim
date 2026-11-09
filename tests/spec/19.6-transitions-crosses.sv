// IEEE 1800-2023 19.3, 19.5.2, 19.6, 19.11: ref/input constructor arguments,
// sampling with iff, transition bins ([*], [->]), crosses with binsof,
// intersect and ignore_bins, get_inst_coverage(cov, tot), type coverage,
// and $get_coverage.
//! icarus-skip: Icarus 13 does not support covergroups
module top;
  logic clk = 0, en = 0;
  always #5 clk = ~clk;
  bit [3:0] a, b;
  int va, vb;
  covergroup cg (ref int ra, input int low, int high) @(posedge clk iff en);
    coverpoint ra { bins good = {[low:high]}; bins bad[] = default; }
  endgroup
  cg c1 = new(va, 0, 50);
  cg c2 = new(vb, 120, 600);
  covergroup tr @(posedge clk);
    option.per_instance = 1;
    ta: coverpoint a { bins t1 = (1 => 2 => 3); bins t2[] = (4, 5 => 6); bins t3 = (7 [*3]); bins t4 = (8 [-> 2] => 9); }
    cb: coverpoint b { bins lo = {[0:7]}; bins hi = {[8:15]}; }
    x: cross ta, cb;
  endgroup
  tr t = new;
  covergroup cr;
    ca: coverpoint a { bins a0 = {0}; bins a1 = {1}; bins a2 = {2}; }
    cbb: coverpoint b { bins b0 = {0}; bins b1 = {1}; }
    axb: cross ca, cbb {
      bins both0 = binsof(ca.a0) && binsof(cbb.b0);
      ignore_bins ig = binsof(ca) intersect {2};
    }
  endgroup
  cr r = new;
  int cov, tot;
  bit [3:0] seq[] = '{1, 2, 3, 4, 6, 5, 6, 7, 7, 7, 8, 0, 8, 9};
  initial begin
    va = 10; vb = 700;
    @(negedge clk) en = 1;
    @(negedge clk) va = 60; vb = 200;
    @(negedge clk) en = 0;
    $display("c1 %0.2f c2 %0.2f type %0.2f", c1.get_inst_coverage(), c2.get_inst_coverage(), cg::get_coverage());
    foreach (seq[i]) begin a = seq[i]; b = i; @(negedge clk); end
    $display("ta %0.2f x %0.2f", t.ta.get_inst_coverage(), t.x.get_inst_coverage());
    void'(t.x.get_inst_coverage(cov, tot));
    $display("x %0d/%0d", cov, tot);
    a = 0; b = 0; r.sample(); a = 1; b = 1; r.sample(); a = 2; b = 0; r.sample();
    void'(r.axb.get_inst_coverage(cov, tot));
    $display("axb %0d/%0d %0.2f", cov, tot, r.axb.get_coverage());
    $display("global %0.2f", $get_coverage());
    $finish;
  end
endmodule

//> c1 100.00 c2 100.00 type 100.00
//> ta 100.00 x 50.00
//> x 5/10
//> axb 2/4 50.00
//> global 88.89
