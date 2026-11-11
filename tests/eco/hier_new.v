// New, flat (as synthesis with ungrouping writes it): stage 1 loads d | e.
module top (input clk, input [1:0] d, input e, output [1:0] q, output any);
  wire n0, n1;
  AND2_X1 \s0/g (.A(d[0]), .B(e), .Y(n0));
  DFF_X1 \s0/r (.CK(clk), .D(n0), .Q(q[0]));
  OR2_X1 \s1/g (.A(d[1]), .B(e), .Y(n1));
  DFF_X1 \s1/r (.CK(clk), .D(n1), .Q(q[1]));
  OR2_X1 o (.A(q[0]), .B(q[1]), .Y(any));
endmodule
