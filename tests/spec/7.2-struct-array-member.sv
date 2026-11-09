// IEEE 1800-2023 7.2: an unpacked struct can have members of any data
// type, including fixed-size unpacked arrays of integral, real and struct
// elements: element reads and writes (constant and variable index), whole
// member and whole struct assignment patterns, copies and equality, foreach,
// %p (21.2.1.6), and default values (6.8).
//! icarus-skip: Icarus 13 does not support arrays inside unpacked structs
module top;
  typedef struct { int x; logic [7:0] y; } p_t;
  typedef struct { int a; bit [3:0] arr[2]; real r[2]; p_t ps[0:1]; byte m[2][3]; } s_t;
  typedef struct { logic [1:0] l[2]; int n; } d_t;
  s_t s, u, v[3];
  d_t d;
  int i;
  initial begin
    s.a = 1;
    s.arr = '{4'h5, 4'h6};
    s.r[0] = 1.5; s.r[1] = 2.25;
    s.ps[1].x = 7; s.ps[1].y = 8'hab; s.ps[0] = '{x: 3, y: 4};
    for (i = 0; i < 2; i++) for (int j = 0; j < 3; j++) s.m[i][j] = 8'(i * 10 + j);
    $display("%0d %0d %0d %0d %h %0d", s.arr[0], s.arr[1], s.ps[0].x, s.ps[1].x, s.ps[1].y, s.m[1][2]);
    $display("%0.2f %0.2f", s.r[0], s.r[1]);
    u = s;
    $display("%0d %0d", u == s, u.arr[1]);
    u.arr[i - 2] = 4'hf;
    $display("%h %0d", u.arr[0], u == s);
    v[2] = '{a: 9, arr: '{1, 2}, r: '{0.5, 0.25}, ps: '{'{1, 2}, '{3, 4}}, m: '{'{1, 2, 3}, '{4, 5, 6}}};
    $display("%0d %0d %0.2f %0d %0d", v[2].a, v[2].arr[1], v[2].r[1], v[2].ps[1].y, v[2].m[1][2]);
    foreach (s.arr[k]) $display("arr[%0d]=%0d", k, s.arr[k]);
    $display("%p", d);
    d.l = '{2{2'b10}};
    $display("%p", d);
  end
endmodule
//> 5 6 3 7 ab 12
//> 1.50 2.25
//> 1 6
//> f 0
//> 9 2 0.25 4 6
//> arr[0]=5
//> arr[1]=6
//> '{l:'{x, x}, n:0}
//> '{l:'{2, 2}, n:0}
