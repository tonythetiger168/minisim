// IEEE 1800-2023 25.5.4: modport expressions. A modport port can name an
// expression of the interface's signals: a part select, a whole signal
// under another name, or a concatenation. Two modports split one vector.
//! icarus-skip: Icarus 13 has no modport expressions
interface split_if;
  logic [7:0] r;
  logic [3:0] a, b;
  modport lo (output .P(r[3:0]), input .Q(r), output .W({a, b}));
  modport hi (output .P(r[7:4]), input .Q(r));
endinterface
module low_side (split_if.lo s);
  initial begin
    #1 s.P = 4'h5;
    s.P[3] = 1'b1;
    s.W = 8'hc3;
  end
endmodule
module high_side (split_if.hi s);
  initial begin
    #2 s.P = 4'ha;
    #1 $display("Q=%h P[5]=%b", s.Q, s.P[5]);
  end
  always @(s.Q) $display("%0t r=%h", $time, s.Q);
endmodule
module top;
  split_if x();
  low_side u1(x.lo);
  high_side u2(x.hi);
  initial #5 $display("r=%h a=%h b=%h", x.r, x.a, x.b);
endmodule
//> 1 r=xd
//> 2 r=ad
//> Q=ad P[5]=1
//> r=ad a=c b=3
