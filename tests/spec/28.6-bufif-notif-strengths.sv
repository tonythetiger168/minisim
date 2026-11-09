// IEEE 1800-2023 28.6: bufif0, bufif1, notif0, notif1. The control enables
// the output; disabled, it is Z; an X or Z control gives L or H (shown as
// x), which other drivers of the net resolve by strength (28.12).
module top;
  reg d, c;
  wire b1, b0, n1, n0;
  bufif1 g1(b1, d, c);
  bufif0 g2(b0, d, c);
  notif1 g3(n1, d, c);
  notif0 g4(n0, d, c);
  reg cx = 1'bx;
  wire la, hb, le;
  bufif1 l1(la, 1'b0, cx);  buf l2(la, 1'b0);                  // L + St0: 0
  bufif1 h1(hb, 1'b1, cx);  buf h2(hb, 1'b1);                  // H + St1: 1
  bufif1 e1(le, 1'b0, cx);  buf (weak0, weak1) e2(le, 1'b1);   // L + We1: x
  wire bus;
  bufif1 t1(bus, 1'b1, c);
  bufif0 t2(bus, 1'b0, c);
  integer i, j;
  reg [3:0] v = 4'b01xz;
  initial begin
    for (i = 0; i < 4; i++)
      for (j = 0; j < 4; j++) begin
        d = v[3-i]; c = v[3-j];
        #1 $display("d=%b c=%b %b %b %b %b bus=%b", d, c, b1, b0, n1, n0, bus);
      end
    $display("%b %b %b", la, hb, le);
  end
endmodule
//> d=0 c=0 z 0 z 1 bus=0
//> d=0 c=1 0 z 1 z bus=1
//> d=0 c=x x x x x bus=x
//> d=0 c=z x x x x bus=x
//> d=1 c=0 z 1 z 0 bus=0
//> d=1 c=1 1 z 0 z bus=1
//> d=1 c=x x x x x bus=x
//> d=1 c=z x x x x bus=x
//> d=x c=0 z x z x bus=0
//> d=x c=1 x z x z bus=1
//> d=x c=x x x x x bus=x
//> d=x c=z x x x x bus=x
//> d=z c=0 z x z x bus=0
//> d=z c=1 x z x z bus=1
//> d=z c=x x x x x bus=x
//> d=z c=z x x x x bus=x
//> 0 1 x
