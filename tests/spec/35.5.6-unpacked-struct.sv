// IEEE 1800-2023 35.5.6 / H.7.6: an unpacked struct DPI argument is a C
// struct with the same members in the same order, passed by pointer (a
// const pointer for an input). Members may be scalars, reals, packed
// vectors, nested structs and fixed-size arrays.
//! args: data/dpi/ustruct.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  typedef struct { shortint x, y; } point_t;
  typedef struct {
    byte     id;
    point_t  at;
    int      hist[3];
    real     gain;
    shortreal bias;
    bit      on;
    bit [40:0] mask;
    logic [3:0] nib;
    longint  big;
  } rec_t;
  import "DPI-C" function int c_check(input rec_t r);
  import "DPI-C" function void c_make(input int seed, output rec_t r);
  import "DPI-C" function void c_bump(inout point_t p);
  rec_t a, b;
  point_t p = '{-3, 9};
  initial begin
    a = '{id: -5, at: '{100, -200}, hist: '{1, 2, 3}, gain: 2.5, bias: -0.75,
          on: 1, mask: 41'h1_2345_6789A, nib: 4'b1x0z, big: -64'sd123456789012};
    $display("check %0d", c_check(a));
    c_make(7, b);
    $display("id=%0d at=(%0d,%0d) hist=%p gain=%0.2f bias=%0.2f on=%b mask=%h nib=%b big=%0d",
             b.id, b.at.x, b.at.y, b.hist, b.gain, b.bias, b.on, b.mask, b.nib, b.big);
    c_bump(p);
    c_bump(p);
    $display("p=(%0d,%0d)", p.x, p.y);
  end
endmodule
//> check 1
//> id=7 at=(8,-9) hist='{70, 71, 72} gain=3.50 bias=0.50 on=0 mask=10000000003 nib=z1x0 big=-7
//> p=(-1,11)
