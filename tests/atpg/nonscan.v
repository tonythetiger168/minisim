// Three scan flops and one flop the tester cannot scan (its clock is gated
// by logic): the tool must report it, read it as unknown, and still test the
// rest. The design is hierarchical; the stage module is flattened.
module stage (input d, input si, input se, input ck, input rn, output q);
  wire n;
  INV u_i (.A(d), .Y(n));
  SDFFR u_f (.D(n), .SI(si), .SE(se), .CK(ck), .RN(rn), .Q(q));
endmodule

module nonscan (input clk, input rst_n, input en, input scan_en, input scan_in,
                input [2:0] a, output scan_out, output [1:0] y);
  wire q0, q1, q2, qg, gclk, m, k;
  stage s0 (.d(a[0]), .si(scan_in), .se(scan_en), .ck(clk), .rn(rst_n), .q(q0));
  stage s1 (.d(m), .si(q0), .se(scan_en), .ck(clk), .rn(rst_n), .q(q1));
  stage s2 (.d(k), .si(q1), .se(scan_en), .ck(clk), .rn(rst_n), .q(q2));
  AND2 u_gate (.A(clk), .B(en), .Y(gclk));
  DFF u_gated (.D(a[2]), .CK(gclk), .Q(qg));
  XOR2 u_m (.A(q0), .B(a[1]), .Y(m));
  AOI21 u_k (.A(q1), .B(qg), .C(a[2]), .Y(k));
  NAND2 u_y0 (.A(q2), .B(q0), .Y(y[0]));
  OR2 u_y1 (.A(qg), .B(q1), .Y(y[1]));
  BUF u_so (.A(q2), .Y(scan_out));
endmodule
