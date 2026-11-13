// IEEE 1800-2023 19.3 / 6.8: a covergroup created by a declaration
// initializer (cg_t cg = new()) exists from time 0, also when a VPI
// application is loaded, which makes declaration initializers run before
// the simulation starts. The final block reads its coverage.
//! args: data/vpi/systf.c
//! icarus-skip: Icarus 13 does not support covergroups
interface bus_if(input logic clk);
  logic a;
  covergroup cg_bus @(posedge clk);
    cp_a: coverpoint a { bins off = {0}; bins on = {1}; }
  endgroup
  cg_bus cg = new();
  initial #1 $display("cg created: %0d", cg != null);
  final $display("coverage %0.1f, cp_a %0.1f", cg.get_inst_coverage(), cg.cp_a.get_inst_coverage());
endinterface
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  bus_if b0(clk);
  initial begin b0.a = 0; #10 b0.a = 1; #10 $finish; end
endmodule
//> cg created: 1
//> coverage 100.0, cp_a 100.0
