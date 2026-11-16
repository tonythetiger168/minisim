// IEEE 1800-2023 18.5 and 18.7: a constraint may read a fixed-size array
// property of another object through its handle: as the item list of
// inside (also an array of arrays), with constant element selects and with
// a foreach index. The values are read when randomize() runs, so a later
// change to the array is seen.
//! icarus-skip: Icarus 13 has no constraints
class Table;
  int vals[4];
  bit [7:0] grid[2][3];
endclass

class Pick;
  Table t;
  rand int v;
  rand bit [7:0] g;
  constraint c { v inside {t.vals}; g inside {t.grid}; g != t.grid[0][0]; }
endclass

class Under;
  Table t;
  rand bit [7:0] a[4];
  constraint c { foreach (a[i]) a[i] == t.vals[i] + i; }
endclass

module top;
  initial begin
    automatic Table tb = new;
    automatic Pick p = new;
    automatic int ok = 1;
    automatic int x;
    tb.vals = '{11, 22, 33, 44};
    tb.grid = '{'{1, 2, 3}, '{4, 5, 6}};
    p.t = tb;
    repeat (20) begin
      if (!p.randomize()) ok = 0;
      if (!(p.v inside {11, 22, 33, 44}) || !(p.g inside {[2:6]})) ok = 0;
    end
    $display("first %0d", ok);
    tb.vals = '{7, 7, 7, 7};
    void'(p.randomize());
    $display("v=%0d", p.v);
    repeat (10) begin
      if (!std::randomize(x) with { x inside {tb.vals}; }) ok = 0;
      if (x != 7) ok = 0;
    end
    $display("second %0d", ok);
    begin
      automatic Under u = new;
      u.t = tb;
      tb.vals = '{10, 20, 30, 40};
      void'(u.randomize());
      $display("%p", u.a);
    end
  end
endmodule
//> first 1
//> v=7
//> second 1
//> '{10, 21, 32, 43}
