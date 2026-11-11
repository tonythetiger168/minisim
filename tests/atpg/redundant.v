// y = a | (a & b): the AND is redundant, so some of its faults cannot be
// tested and must be reported as undetectable, not as coverage lost.
module redundant (input a, input b, input c, output y, output z);
  wire t;
  AND2 u_and (.A(a), .B(b), .Y(t));
  OR2 u_or (.A(a), .B(t), .Y(y));
  XOR2 u_x (.A(b), .B(c), .Y(z));
endmodule
