// IEEE 1800-2023 20.13 / 40.3.2: $coverage_control, $coverage_get_max,
// $coverage_get, $coverage_save and $coverage_merge, over an instance
// (top.u1), all instances of a module ("leaf") or $root, with or without
// the hierarchy below. Toggle and statement coverage come from msim -cov;
// assertion coverage counts assertions with at least one success. A
// stopped instance keeps what it had at the stop; a reset clears it; a
// merge adds a saved database back.
//! args: -cov spec_20_13.cov.json
//! icarus-skip: Icarus 13 has no coverage functions
module leaf(input logic clk, input logic [1:0] d);
  logic [1:0] q;
  always @(posedge clk) begin
    q <= d;
    if (d == 3) q <= 0;
  end
  a_ok: assert property (@(posedge clk) d != 2);
endmodule
module top;
  logic clk = 0;
  logic [1:0] d1 = 0, d2 = 0;
  leaf u1(clk, d1);
  leaf u2(clk, d2);
  always #5 clk = ~clk;
  initial begin
    $display("check stmt u1 %0d", $coverage_control(`SV_COV_CHECK, `SV_COV_STATEMENT, `SV_COV_MODULE, top.u1));
    $display("check bad %0d", $coverage_control(`SV_COV_CHECK, `SV_COV_STATEMENT, `SV_COV_MODULE, "nosuch"));
    $display("toggle max leaf %0d", $coverage_get_max(`SV_COV_TOGGLE, `SV_COV_MODULE, "leaf"));
    $display("assert max all %0d", $coverage_get_max(`SV_COV_ASSERTION, `SV_COV_HIER, $root));
    #12 d1 = 3;
    #20 d1 = 0;
    $display("toggle u1 %0d u2 %0d", $coverage_get(`SV_COV_TOGGLE, `SV_COV_MODULE, top.u1),
             $coverage_get(`SV_COV_TOGGLE, `SV_COV_MODULE, top.u2));
    $display("assert covered %0d", $coverage_get(`SV_COV_ASSERTION, `SV_COV_HIER, top));
    $display("save %0d", $coverage_save(`SV_COV_TOGGLE, "spec_20_13"));
    $display("reset %0d", $coverage_control(`SV_COV_RESET, `SV_COV_TOGGLE, `SV_COV_HIER, top.u1));
    $display("toggle u1 after reset %0d", $coverage_get(`SV_COV_TOGGLE, `SV_COV_MODULE, top.u1));
    $display("stop %0d", $coverage_control(`SV_COV_STOP, `SV_COV_TOGGLE, `SV_COV_MODULE, top.u2));
    d2 = 3; #20 d2 = 0; #20;
    $display("toggle u2 while stopped %0d", $coverage_get(`SV_COV_TOGGLE, `SV_COV_MODULE, top.u2));
    $display("start %0d", $coverage_control(`SV_COV_START, `SV_COV_TOGGLE, `SV_COV_MODULE, top.u2));
    $display("merge %0d", $coverage_merge(`SV_COV_TOGGLE, "spec_20_13"));
    $display("toggle u1 after merge %0d", $coverage_get(`SV_COV_TOGGLE, `SV_COV_MODULE, top.u1));
    $display("merge missing %0d", $coverage_merge(`SV_COV_TOGGLE, "nosuch"));
    $finish;
  end
endmodule
//> check stmt u1 1
//> check bad -1
//> toggle max leaf 10
//> assert max all 2
//> toggle u1 3 u2 1
//> assert covered 2
//> save 1
//> reset 1
//> toggle u1 after reset 0
//> stop 1
//> toggle u2 while stopped 1
//> start 1
//> merge 1
//> toggle u1 after merge 5
//> merge missing -1
