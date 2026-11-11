// A 3-bit accumulator on the cells of tiny.lib: a full adder (two outputs),
// a scan flop with its own pin names, and a chain that runs through QN once,
// so the scan data is inverted on the way.
module libscan (input ck, input rstn, input te, input ti, input [2:0] d, input sel,
                output to, output [2:0] sum, output cout);
  wire q0, q1, q2, q0n, s0, s1, s2, c0, c1, c2, m0, x;
  FAX1 fa0 (.A(q0), .B(d[0]), .CI(1'b0), .S(s0), .CO(c0));
  FAX1 fa1 (.A(q1), .B(d[1]), .CI(c0), .S(s1), .CO(c1));
  FAX1 fa2 (.A(q2), .B(d[2]), .CI(c1), .S(s2), .CO(c2));
  MX2X1 mx (.I0(s0), .I1(d[0]), .S(sel), .Z(m0));
  SDFRQX1 r0 (.D(m0), .TI(ti), .TE(te), .CP(ck), .RDN(rstn), .Q(q0), .QN(q0n));
  SDFRQX1 r1 (.D(s1), .TI(q0n), .TE(te), .CP(ck), .RDN(rstn), .Q(q1));
  SDFRQX1 r2 (.D(s2), .TI(q1), .TE(te), .CP(ck), .RDN(rstn), .Q(q2));
  ND2X1 nd (.A1(q2), .A2(c2), .ZN(x));
  INVX1 iv (.I(x), .ZN(cout));
  assign sum = {q2, q1, q0};
  assign to = q2;
endmodule
