// A 4-bit counter with enable and reset as a gate-level netlist, the way a
// synthesis tool writes one: a submodule, bus ports, a constant, an alias.
// Cells are those of tiny.lef.
module half_add (input a, input b, output s, output c);
  XOR2 u_s (.A(a), .B(b), .Y(s));
  AND2 u_c (.A(a), .B(b), .Y(c));
endmodule

module counter (input clk, input rst_n, input en, output [3:0] q, output carry, output one);
  wire [3:0] d, s;
  wire [4:0] c;
  assign c[0] = en;
  assign carry = c[4];
  assign one = 1'b1;
  half_add ha0 (.a(q[0]), .b(c[0]), .s(s[0]), .c(c[1]));
  half_add ha1 (.a(q[1]), .b(c[1]), .s(s[1]), .c(c[2]));
  half_add ha2 (.a(q[2]), .b(c[2]), .s(s[2]), .c(c[3]));
  half_add ha3 (.a(q[3]), .b(c[3]), .s(s[3]), .c(c[4]));
  // d = s unless the low bits are being cleared by the constant 0 below.
  MUX2 m0 (.A(s[0]), .B(1'b0), .S(1'b0), .Y(d[0]));
  BUF  b1 (.A(s[1]), .Y(d[1]));
  BUF  b2 (.A(s[2]), .Y(d[2]));
  BUF  b3 (.A(s[3]), .Y(d[3]));
  DFFR r0 (.D(d[0]), .CK(clk), .RN(rst_n), .Q(q[0]));
  DFFR r1 (.D(d[1]), .CK(clk), .RN(rst_n), .Q(q[1]));
  DFFR r2 (.D(d[2]), .CK(clk), .RN(rst_n), .Q(q[2]));
  DFFR r3 (.D(d[3]), .CK(clk), .RN(rst_n), .Q(q[3]));
endmodule
