// The implemented netlist: scan flops, a clock buffer, a spare gate.
module top (clk, rst_n, scan_en, scan_in, a, b, c, y, z, scan_out);
  input clk, rst_n, scan_en, scan_in;
  input [1:0] a;
  input b, c;
  output y, z, scan_out;
  wire n1, n2, n3, n4, q0, q1, cclk, spare;
  BUF_X1 ckbuf (.A(clk), .Y(cclk));
  AND2_X1 u1 (.A(a[0]), .B(a[1]), .Y(n1));
  XOR2_X1 u2 (.A(n1), .B(b), .Y(n2));
  NAND2_X1 u3 (.A(n2), .B(c), .Y(n3));
  INV_X1 u4 (.A(n3), .Y(n4));
  SDFFR_X1 r0 (.CK(cclk), .D(n4), .SI(scan_in), .SE(scan_en), .RN(rst_n), .Q(q0));
  SDFFR_X1 r1 (.CK(cclk), .D(n2), .SI(q0), .SE(scan_en), .RN(rst_n), .Q(q1));
  OR2_X1 u5 (.A(q0), .B(q1), .Y(y));
  assign z = q1;
  assign scan_out = q1;
  NAND2_X1 spare0 (.A(1'b0), .B(1'b0), .Y(spare));
endmodule
