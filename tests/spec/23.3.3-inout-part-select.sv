// IEEE 1800-2023 23.3.3, 23.3.3.7, 6.6.4: inout ports joined to a bit, a part
// of a pulled-up bus, or a whole net of another net type. Each side sees the
// other's drivers and the bus pull-up, and a released driver lets the bus go
// back up (nothing holds the last driven value).
module od_dev(inout wire pin, input logic lo);
  assign pin = lo ? 1'b0 : 1'bz;
endmodule
module od_pair(inout wire [1:0] p, input logic [1:0] lo);
  assign p[0] = lo[0] ? 1'b0 : 1'bz;
  assign p[1] = lo[1] ? 1'b0 : 1'bz;
endmodule
module od_host(inout logic [5:0] bus, input logic lo);
  assign bus[5] = lo ? 1'b0 : 1'bz;
endmodule
module top;
  tri1 [5:0] bus;
  logic [5:0] up_lo = 0;
  logic a = 0, h = 0;
  logic [1:0] pr = 0;
  od_dev  d0(.pin(bus[0]), .lo(a));
  od_pair d1(.p(bus[3:2]), .lo(pr));
  od_host hs(.bus(bus), .lo(h));
  assign bus[4] = up_lo[4] ? 1'b0 : 1'bz;
  initial begin
    #1 $display("idle %b pin %b p %b host %b", bus, d0.pin, d1.p, hs.bus);
    a = 1;
    #1 $display("bit0 %b pin %b host %b", bus, d0.pin, hs.bus);
    a = 0; pr = 2'b10;
    #1 $display("part %b p %b", bus, d1.p);
    pr = 0; h = 1;
    #1 $display("host %b pin %b", bus, d0.pin);
    h = 0; up_lo[4] = 1;
    #1 $display("top  %b host %b", bus, hs.bus);
    up_lo = 0;
    #1 $display("free %b pin %b p %b host %b", bus, d0.pin, d1.p, hs.bus);
  end
endmodule
//> idle 111111 pin 1 p 11 host 111111
//> bit0 111110 pin 0 host 111110
//> part 110111 p 01
//> host 011111 pin 1
//> top  101111 host 101111
//> free 111111 pin 1 p 11 host 111111
