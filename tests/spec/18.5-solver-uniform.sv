// IEEE 1800-2023 18.5.10 (and 18.5): the solver picks values uniformly over
// the joint solution space, not variable by variable. `s -> d == 0` has 2^32
// solutions with s == 0 and one with s == 1, so s comes out 0. With
// `solve s before d` (18.5.9) s is picked first and is 1 about half the time.
// x < y on 8-bit values: P(x < 64) is 14304/32640 = 0.438. A range plus a
// single value (addr inside {[a:b], c}) weights the value by how many data
// values it allows (data < addr * 2), so c is rare.
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class Imp;
    rand bit s; rand bit [31:0] d;
    constraint c { s -> d == 0; }
  endclass
  class ImpBefore;
    rand bit s; rand bit [31:0] d;
    constraint c { s -> d == 0; solve s before d; }
  endclass
  class Lt;
    rand bit [7:0] x, y;
    constraint c { x < y; }
  endclass
  class Bus;
    rand bit [15:0] addr; rand bit [31:0] data;
    constraint c { addr[1:0] == 0; addr inside {[16'h1000:16'h1fff], 16'h8000}; data < addr * 2; }
  endclass
  Imp a = new; ImpBefore b = new; Lt l = new; Bus u = new;
  int s1, s1b, lowx, hi, lowd;
  initial begin
    repeat (1000) begin
      void'(a.randomize()); s1 += a.s;
      void'(b.randomize()); s1b += b.s;
      void'(l.randomize()); if (l.x < 64) lowx++;
      void'(u.randomize()); if (u.addr == 16'h8000) hi++; if (u.data < 16'h2000) lowd++;
    end
    // 0, ~500, ~438, ~5, ~667
    $display("%0d %0d %0d %0d %0d", s1 < 5, s1b > 400 && s1b < 600, lowx > 380 && lowx < 500, hi < 25,
             lowd > 600 && lowd < 730);
  end
endmodule

//> 1 1 1 1 1
