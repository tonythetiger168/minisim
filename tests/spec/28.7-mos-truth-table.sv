// IEEE 1800-2023 28.7, 28.9: nmos, pmos, rnmos, rpmos, cmos, rcmos. A switch
// passes its data (Z too) while its control is on and is Z while it is
// off; cmos(o, d, c, ~c) is on when c is 1, off when c is 0. A resistive switch drives at most pull strength (28.13.1), so a
// strong driver on the same net wins.
module top;
  reg d, c;
  wire nm, pm, cm, rn, rp, rc;
  nmos  m1(nm, d, c);
  pmos  m2(pm, d, c);
  cmos  m3(cm, d, c, ~c);
  rnmos m4(rn, d, c);
  rpmos m5(rp, d, c);
  rcmos m6(rc, d, c, ~c);
  wire w;
  rnmos r1(w, 1'b1, 1'b1);
  buf   b1(w, 1'b0);   // strong 0 beats the pull 1 through the resistive switch
  integer i, j;
  reg [3:0] v = 4'b01xz;
  initial begin
    for (i = 0; i < 4; i++)
      for (j = 0; j < 2; j++) begin
        d = v[3-i]; c = j;
        #1 $display("d=%b c=%b %b %b %b %b %b %b", d, c, nm, pm, cm, rn, rp, rc);
      end
    $display("w=%b", w);
  end
endmodule
//> d=0 c=0 z 0 z z 0 z
//> d=0 c=1 0 z 0 0 z 0
//> d=1 c=0 z 1 z z 1 z
//> d=1 c=1 1 z 1 1 z 1
//> d=x c=0 z x z z x z
//> d=x c=1 x z x x z x
//> d=z c=0 z z z z z z
//> d=z c=1 z z z z z z
//> w=0
