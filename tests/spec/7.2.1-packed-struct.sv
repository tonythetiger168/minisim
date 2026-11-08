// IEEE 1800-2023 7.2.1, 7.3.1: packed struct and union members are bit slices;
// a member can be read and written, also nested.
module top;
  typedef struct packed { logic [3:0] hi; logic [3:0] lo; } byte_t;
  typedef union packed { byte_t b; logic [7:0] raw; } u_t;
  typedef struct packed { byte_t a; logic [1:0] t; } nest_t;
  byte_t s;
  u_t u;
  nest_t n;
  initial begin
    s.hi = 4'hA; s.lo = 4'h5;
    $display("%h %h %h", s, s.hi, s.lo);
    u.raw = 8'h3C;
    $display("%0h %0h", u.b.hi, u.b.lo);
    u.b.lo = 4'hF;
    $display("%h", u.raw);
    n = 0; n.a.hi = 4'h9; n.t = 2'b11;
    $display("%h %b", n, n.a.hi[3]);
  end
endmodule

//> a5 a 5
//> 3 c
//> 3f
//> 243 1
