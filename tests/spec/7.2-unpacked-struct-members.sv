// IEEE 1800-2023 7.2: Unpacked struct defaults per member (6.8), named and
// default: patterns, nested structs with real and string members, arrays of
// structs, struct function results, and %p (7.2, 10.9.2).
//! icarus-skip: Icarus 13 does not support unpacked structs with string members
module top;
  typedef struct { int a; real r; } in_t;
  typedef struct { logic [3:0] n; bit [3:0] m; in_t in; string s; } s_t;
  s_t x, y;
  s_t arr[2];
  function automatic s_t mk(int v);
    s_t t;
    t.n = v; t.m = v; t.in.a = v * 2; t.in.r = v / 2.0; t.s = "f";
    return t;
  endfunction
  initial begin
    $display("%b %b %0d %f [%s]", x.n, x.m, x.in.a, x.in.r, x.s);
    x = '{n: 4'h3, default: 0, s: "q"};
    $display("%h %h %0d %f %s", x.n, x.m, x.in.a, x.in.r, x.s);
    x.in.r = 2.5; x.in.r = x.in.r * 2;
    $display("%f", x.in.r);
    arr[1] = mk(5);
    y = arr[1];
    $display("%0d %0d %f %s %0d", y.n, y.in.a, y.in.r, y.s, y == arr[1]);
    $display("%p", y);
  end
endmodule

//> xxxx 0000 0 0.000000 []
//> 3 0 0 0.000000 q
//> 5.000000
//> 5 10 2.500000 f 1
//> '{n:5, m:5, in:'{a:10, r:2.50000}, s:"f"}
