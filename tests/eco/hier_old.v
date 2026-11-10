// Old, hierarchical: the patch crosses the module boundary, so the netlist
// is written back flat.
module stage (input clk, input d, input e, output q);
  wire n;
  AND2_X1 g (.A(d), .B(e), .Y(n));
  DFF_X1 r (.CK(clk), .D(n), .Q(q));
endmodule
module top (input clk, input [1:0] d, input e, output [1:0] q, output any);
  stage s0 (.clk(clk), .d(d[0]), .e(e), .q(q[0]));
  stage s1 (.clk(clk), .d(d[1]), .e(e), .q(q[1]));
  OR2_X1 o (.A(q[0]), .B(q[1]), .Y(any));
endmodule
