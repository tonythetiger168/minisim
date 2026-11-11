// Gated clocks and pulse widths.
//   clk -> cb -> g1 (AND2, enable from the falling-edge rn) -> r1
//             -> g2 (OR2, enable from the rising-edge re)   -> r2
//             -> g3 (AND2, enable from the rising-edge re)  -> r3
//             -> ci (INV) -> rf (falling-edge register)
module gate (input clk, input en, input d, output [3:0] q);
  wire c, gc1, gc2, gc3, cn, e_n, e_r;
  BUF_X1 cb (.A(clk), .Y(c));
  DFFN_X1 rn (.CK(c), .D(en), .Q(e_n));
  DFF_X1 re (.CK(c), .D(en), .Q(e_r));
  AND2_X1 g1 (.A(c), .B(e_n), .Y(gc1));
  OR2_X1 g2 (.A(c), .B(e_r), .Y(gc2));
  AND2_X1 g3 (.A(c), .B(e_r), .Y(gc3));
  INV_X1 ci (.A(c), .Y(cn));
  DFF_X1 r1 (.CK(gc1), .D(d), .Q(q[0]));
  DFF_X1 r2 (.CK(gc2), .D(d), .Q(q[1]));
  DFF_X1 r3 (.CK(gc3), .D(d), .Q(q[2]));
  DFFN_X1 rf (.CK(cn), .D(d), .Q(q[3]));
endmodule
