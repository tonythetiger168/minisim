// IEEE 1800-2023 23.3.3.7: an inout port joins two nets into one simulated
// net whose type comes from Table 23-1. A wire or tri on one side takes the
// type of the other: an inout tri port on a tri1 bus is pulled up when no
// one drives it, and so is a tri1 port on a plain wire.
module od_dev(input logic pull_low, inout tri line);
  assign line = pull_low ? 1'b0 : 1'bz;
endmodule
module pu_dev(input logic pull_low, inout tri1 line);
  assign line = pull_low ? 1'b0 : 1'bz;
endmodule
module wa_dev(input logic v, inout wire line);
  assign line = v;
endmodule
module top;
  tri1 bus;        // open-drain bus with a pull-up
  wire w;          // takes the tri1 of pu_dev's port
  wand a;          // a wire port on a wand net is wand
  logic m = 0, d = 0, e = 0;
  logic v1 = 1, v2 = 1;
  assign bus = m ? 1'b0 : 1'bz;
  od_dev u1(.pull_low(d), .line(bus));
  pu_dev u2(.pull_low(e), .line(w));
  wa_dev u3(.v(v1), .line(a));
  wa_dev u4(.v(v2), .line(a));
  initial begin
    #1 $display("idle %b %b %b", bus, u1.line, w);
    d = 1; e = 1;
    #1 $display("low  %b %b %b", bus, u1.line, w);
    d = 0; m = 1; e = 0;
    #1 $display("host %b %b %b", bus, u1.line, w);
    m = 0;
    #1 $display("free %b %b %b", bus, u1.line, w);
    $display("wand %b", a);
    v2 = 0;
    #1 $display("wand %b %b", a, u3.line);
  end
endmodule
//> idle 1 1 1
//> low  0 0 0
//> host 0 0 1
//> free 1 1 1
//> wand 1
//> wand 0 0
