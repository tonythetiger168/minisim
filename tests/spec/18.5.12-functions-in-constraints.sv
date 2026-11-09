// IEEE 1800-2023 18.5.12: functions in constraints. The rand variables a
// call reads are solved before the other variables of its constraint, and
// the call runs with their values: methods, static and package-level
// functions, nested calls, calls in an if guard, with an index of a foreach
// over a rand dynamic array, with rand array elements as arguments, and in
// an inline with {...} reading the caller's variables.
//! icarus-skip: Icarus 13 does not support classes
function automatic int sq(int x); return x * x; endfunction
class C;
  rand bit [7:0] a, b, c;
  rand bit [3:0] m;
  rand bit [7:0] w[];
  rand bit [7:0] v[];
  rand int unsigned x;
  int tbl[] = '{3, 7, 11, 13};
  function int dbl(int y); return 2 * y; endfunction
  function int at(int i); return tbl[i]; endfunction
  static function int add1(int y); return y + 1; endfunction
  constraint c1 { a < 30; b == dbl(dbl(a)) % 256; }
  constraint c2 { m < 4; c == at(m); }
  constraint c3 { if (dbl(m) > 4) a > add1(9); }
  constraint c4 { w.size() == 4; foreach (w[i]) w[i] == at(i) + 1; }
  constraint c5 { v.size() == 3; foreach (v[i]) { v[i] < 50; if (i > 0) v[i] == dbl(v[i-1]) % 256; } }
  constraint c6 { x < 100; sq(x) > 50; }
endclass

module top;
  C o = new;
  int bad, k = 5;
  initial begin
    repeat (40) begin
      if (!o.randomize()) bad++;
      if (o.b != (4 * o.a) % 256 || o.c != o.tbl[o.m] || (o.m > 2 && o.a <= 10)) bad++;
      foreach (o.w[i]) if (o.w[i] != o.tbl[i] + 1) bad++;
      if (o.v.size() != 3 || o.v[1] != (2 * o.v[0]) % 256 || o.v[2] != (2 * o.v[1]) % 256) bad++;
      if (o.x >= 100 || o.x * o.x <= 50) bad++;
    end
    $display("bad %0d", bad);
    if (!o.randomize() with { a == sq(k); }) $display("fail");
    $display("%0d %0d", o.a, o.b);
  end
endmodule

//> bad 0
//> 25 100
