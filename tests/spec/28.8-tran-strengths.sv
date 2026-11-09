// IEEE 1800-2023 28.8, 28.13: tran, rtran, tranif0, tranif1 join nets both
// ways while on. Each net sees every driver of the nets it is joined to, a
// driver seen through an rtran one strength lower; a tranif with an X
// control gives X on the bits where on and off would differ.
module top;
  reg da = 1'bz, db = 1'bz, en = 1;
  wire a, b, c, d;
  assign (weak0, weak1) a = da;
  assign b = db;
  tranif1 t1(a, b, en);
  rtran r1(b, c);
  pullup p(d);
  tranif0 t0(c, d, en);
  initial begin
    #1 $display("a=%b b=%b c=%b d=%b", a, b, c, d);
    da = 0; #1 $display("a=%b b=%b c=%b d=%b", a, b, c, d);
    db = 1; #1 $display("a=%b b=%b c=%b d=%b", a, b, c, d);
    en = 0; #1 $display("a=%b b=%b c=%b d=%b", a, b, c, d);
    db = 1'bz; #1 $display("a=%b b=%b c=%b d=%b", a, b, c, d);
    db = 0; #1 $display("a=%b b=%b c=%b d=%b", a, b, c, d);
    en = 1'bx; #1 $display("a=%b b=%b c=%b d=%b", a, b, c, d);
  end
endmodule
//> a=z b=z c=z d=1
//> a=0 b=0 c=0 d=1
//> a=1 b=1 c=1 d=1
//> a=0 b=1 c=1 d=1
//> a=0 b=1 c=1 d=1
//> a=0 b=0 c=x d=x
//> a=0 b=0 c=x d=x
