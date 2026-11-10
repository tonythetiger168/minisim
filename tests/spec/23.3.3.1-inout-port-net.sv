// IEEE 1800-2023 23.3.3.1: an inout port connected to a net is that net.
// Drivers on both sides resolve on one net, and a driver that turns off
// (z) leaves nothing behind.
module pad(inout wire [1:0] d, input logic en, input logic [1:0] v, output logic [1:0] seen);
  assign d[1] = en ? v[1] : 1'bz;
  assign d[0] = en ? v[0] : 1'bz;
  assign seen = d;
endmodule
module top;
  logic [1:0] drv = 'z;
  logic en = 0;
  logic [1:0] v = 2'b10;
  wire [1:0] seen;
  wire [1:0] d = drv;
  pad u(.d(d), .en(en), .v(v), .seen(seen));
  initial begin
    #1 $display("%b %b", d, seen);
    en = 1;        #1 $display("%b %b", d, seen);
    en = 0;        #1 $display("%b %b", d, seen);
    drv = 2'b01;   #1 $display("%b %b", d, seen);
    drv[0] = 1'bz; #1 $display("%b %b", d, seen);
    en = 1;        #1 $display("%b %b", d, seen);
    drv = 'z;      #1 $display("%b %b %b", d, seen, u.d);
  end
endmodule

//> zz zz
//> 10 10
//> zz zz
//> 01 01
//> 0z 0z
//> x0 x0
//> 10 10 10
