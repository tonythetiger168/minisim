// IEEE 1800-2023 23.3.3.7 / 11.4.12: an inout port connected to a
// concatenation of net bits (as gate netlists write `.pad({p[1], p[0]})`)
// joins each port bit to the matching net bit in both directions. When the
// child drives z the nets are released, so another driver can take them.
// The result must match a plain whole-net connection (q).
//! icarus-skip: checked against the whole-net connection q
module pc (input logic oe, input logic [3:0] a, inout wire [3:0] pad);
  assign pad = oe ? a : 4'bz;
endmodule
module t;
  logic oe = 0, eoe = 0;
  logic [3:0] a = 0, e = 0;
  wire [3:0] p, q, w;
  wire [1:0] s;
  wire r0, r1;
  pc c0(.oe(oe), .a(a), .pad({p[3:2], p[1], p[0]}));
  pc c1(.oe(oe), .a(a), .pad(q));
  pc c2(.oe(oe), .a(a), .pad({w[0], w[1], w[2], w[3]}));   // reversed bits
  pc c3(.oe(oe), .a(a), .pad({r1, s, r0}));                 // several nets
  assign p = eoe ? e : 4'bz;
  assign q = eoe ? e : 4'bz;
  assign w = eoe ? {e[0], e[1], e[2], e[3]} : 4'bz;
  assign {r1, s, r0} = eoe ? e : 4'bz;
  initial begin
    #1 $display("%b %b %b %b%b%b", p, q, w, r1, s, r0);
    oe = 1; a = 4'b1001;
    #1 $display("%b %b %b %b%b%b", p, q, w, r1, s, r0);
    a = 4'b0110;
    #1 $display("%b %b %b %b%b%b", p, q, w, r1, s, r0);
    oe = 0; eoe = 1; e = 4'b1100;
    #1 $display("%b %b %b %b%b%b", p, q, w, r1, s, r0);
    oe = 1; a = 4'b1010;
    #1 $display("%b %b %b %b%b%b", p, q, w, r1, s, r0);
    oe = 0; eoe = 0;
    #1 $display("%b %b %b %b%b%b", p, q, w, r1, s, r0);
  end
endmodule
//> zzzz zzzz zzzz zzzz
//> 1001 1001 1001 1001
//> 0110 0110 0110 0110
//> 1100 1100 0011 1100
//> 1xx0 1xx0 0xx1 1xx0
//> zzzz zzzz zzzz zzzz
