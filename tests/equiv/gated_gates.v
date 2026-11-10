// gated.sv with the enable moved onto the clock: one integrated clock gate
// (a latch that is open while clk is low, and an AND) feeds four plain flops.
// With `ICG_CELL the gate is a library cell (cells.lib: CKGATE); otherwise it
// is spelled out with a LATCH and an AND2.
module gated (input clk, rst_n, en, scan_en, input [3:0] d, output [3:0] q);
  wire gclk;
`ifdef ICG_CELL
  CKGATE cg (.CK(clk), .E(en), .SE(scan_en), .GCK(gclk));
`else
  wire e, nclk, le;
  OR2 o0 (.A(en), .B(scan_en), .Y(e));
  INV i0 (.A(clk), .Y(nclk));
  LATCH l0 (.D(e), .G(nclk), .Q(le));
`ifdef BUG
  AND2 a0 (.A(clk), .B(en), .Y(gclk));   // no latch: not a clock gate, a glitch
`else
  AND2 a0 (.A(clk), .B(le), .Y(gclk));
`endif
`endif
  DFFR q_reg_0_ (.D(d[0]), .CK(gclk), .RN(rst_n), .Q(q[0]));
  DFFR q_reg_1_ (.D(d[1]), .CK(gclk), .RN(rst_n), .Q(q[1]));
  DFFR q_reg_2_ (.D(d[2]), .CK(gclk), .RN(rst_n), .Q(q[2]));
  DFFR q_reg_3_ (.D(d[3]), .CK(gclk), .RN(rst_n), .Q(q[3]));
endmodule
