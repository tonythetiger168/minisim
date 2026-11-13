// IEEE 1800-2023 23.3.3.7 / 23.3.3.2: an inout port connected to one bit of
// a vector net is a bidirectional connection to that bit; the child's
// driver joins the bit's other drivers and lets go when it drives z.
//! icarus-skip: checked against the scalar connection below
module pc (input logic oe, a, inout wire pad);
  assign pad = oe ? a : 1'bz;
endmodule
module io (inout P, input D);
  assign P = D;
endmodule
module t;
  logic oe = 0, a = 0, eoe = 0, e = 0, d = 1;
  wire [1:0] p;
  wire q, o;
  wire [1:0] v;
  pc c0(.oe(oe), .a(a), .pad(p[0]));
  assign p[0] = eoe ? e : 1'bz;
  pc c1(.oe(oe), .a(a), .pad(q));
  assign q = eoe ? e : 1'bz;
  io b0(.P(o), .D(d));
  io b1(.P(v[1]), .D(d));
  initial begin
    #1 $display("%b %b", p[0], q);
    oe = 1; a = 1;
    #1 $display("%b %b", p[0], q);
    oe = 0; eoe = 1; e = 0;
    #1 $display("%b %b", p[0], q);
    e = 1;
    #1 $display("%b %b", p[0], q);
    eoe = 0;
    #1 $display("%b %b", p[0], q);
    $display("o=%b v=%b", o, v);
    d = 0;
    #1 $display("o=%b v=%b", o, v);
    d = 1;
    #1 $display("o=%b v=%b", o, v);
  end
endmodule
//> z z
//> 1 1
//> 0 0
//> 1 1
//> z z
//> o=1 v=1z
//> o=0 v=0z
//> o=1 v=1z
