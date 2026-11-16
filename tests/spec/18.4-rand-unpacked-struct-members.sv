// IEEE 1800-2023 18.4 and 7.2: in a rand unpacked struct property, the
// members declared rand are random variables solved with the object's
// other variables; members not declared rand keep their values. An enum
// member, also one inside a packed struct member, takes only its named
// values. Arrays and nested unpacked structs of rand members work too.
//! icarus-skip: Icarus 13 has no constraints
typedef enum bit [2:0] { RED = 1, GREEN = 3, BLUE = 6 } color_t;
typedef struct packed { color_t c; bit [79:0] tag; } wide_t;
typedef struct { rand bit [3:0] a; rand bit [3:0] b; } inner_t;
typedef struct {
  rand bit [7:0] x;
  rand color_t col;
  rand bit [3:0] arr[3];
  rand inner_t in;
  rand wide_t wd;
  int keep;
} pkt_t;

class Holder;
  rand pkt_t p;
  rand pkt_t ps[2];
  pkt_t frozen;   // not rand: left alone
  constraint c {
    p.x inside {[10:20]};
    p.in.a < 3;
    foreach (p.arr[i]) p.arr[i] == i + p.in.a;
    ps[1].x == p.x + 1;
  }
endclass

module top;
  initial begin
    automatic Holder h = new;
    automatic int ok = 1, bseen = 0, xseen = 0, cols = 0;
    h.p.keep = 42;
    h.ps[0].keep = 7;
    h.frozen.x = 99;
    repeat (40) begin
      if (!h.randomize()) ok = 0;
      if (!(h.p.x inside {[10:20]}) || h.p.in.a >= 3) ok = 0;
      foreach (h.p.arr[i]) if (h.p.arr[i] != i + h.p.in.a) ok = 0;
      if (h.ps[1].x != h.p.x + 1) ok = 0;
      if (!(h.p.col inside {RED, GREEN, BLUE}) || !(h.p.wd.c inside {RED, GREEN, BLUE})) ok = 0;
      if (!(h.ps[0].col inside {RED, GREEN, BLUE})) ok = 0;
      if (h.p.keep != 42 || h.ps[0].keep != 7 || h.frozen.x != 99) ok = 0;
      bseen |= 1 << h.p.in.b;     // an unconstrained member still changes
      xseen |= 1 << (h.p.x - 10);
      cols |= 1 << h.p.col;
    end
    $display("ok=%0d b-values>4=%0d x-values>4=%0d colors=%b", ok, $countones(bseen) > 4, $countones(xseen) > 4, cols[7:0]);
  end
endmodule
//> ok=1 b-values>4=1 x-values>4=1 colors=01001010
