// misc.sv as gates. Where the RTL says 'x (sel == 3) this netlist reuses the
// sel == 1 arm; the toggle register is called tff here (see -match).
module ram (input clk, we, input [1:0] a, input [3:0] d, output [3:0] q);
  // a hard macro in the real flow: both sides are read with -blackbox ram
endmodule

module misc (input clk, g, we, input [1:0] sel, a, input [3:0] d,
             output [3:0] lat, y, rq, output state_o);
  wire [3:0] nd, m0;
  wire t_next;
  LATCH l0 (.D(d[0]), .G(g), .Q(lat[0]));
  LATCH l1 (.D(d[1]), .G(g), .Q(lat[1]));
  LATCH l2 (.D(d[2]), .G(g), .Q(lat[2]));
  LATCH l3 (.D(d[3]), .G(g), .Q(lat[3]));
  INV i0 (.A(d[0]), .Y(nd[0]));
  INV i1 (.A(d[1]), .Y(nd[1]));
  INV i2 (.A(d[2]), .Y(nd[2]));
  INV i3 (.A(d[3]), .Y(nd[3]));
  // sel[0] ? ~d : d, then sel[1] & ~sel[0] picks the rotation
  MUX2 a0 (.A(d[0]), .B(nd[0]), .S(sel[0]), .Y(m0[0]));
  MUX2 a1 (.A(d[1]), .B(nd[1]), .S(sel[0]), .Y(m0[1]));
  MUX2 a2 (.A(d[2]), .B(nd[2]), .S(sel[0]), .Y(m0[2]));
  MUX2 a3 (.A(d[3]), .B(nd[3]), .S(sel[0]), .Y(m0[3]));
  wire nsel0, rot;
  INV s0 (.A(sel[0]), .Y(nsel0));
`ifdef BUG
  AND2 s1 (.A(sel[1]), .B(sel[0]), .Y(rot));  // rotates on sel == 3, not 2
`else
  AND2 s1 (.A(sel[1]), .B(nsel0), .Y(rot));
`endif
  MUX2 b0 (.A(m0[0]), .B(d[1]), .S(rot), .Y(y[0]));
  MUX2 b1 (.A(m0[1]), .B(d[2]), .S(rot), .Y(y[1]));
  MUX2 b2 (.A(m0[2]), .B(d[3]), .S(rot), .Y(y[2]));
  MUX2 b3 (.A(m0[3]), .B(d[0]), .S(rot), .Y(y[3]));
  ram u_ram (.clk(clk), .we(we), .a(a), .d(d), .q(rq));
  XOR2 t0 (.A(state_o), .B(we), .Y(t_next));
  wire tq;
  DFF tff (.D(t_next), .CK(clk), .Q(tq));
  BUF tb (.A(tq), .Y(state_o));
endmodule
