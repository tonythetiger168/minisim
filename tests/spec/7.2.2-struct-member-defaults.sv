// IEEE 1800-2023 7.2.2: a member of an unpacked struct type may have an
// initial value in the type; every variable of the type starts with it,
// at module level, as an automatic local, as a class property and as a
// member of another struct. A member with none takes its type's default.
module t;
  parameter logic [3:0] C = 4'h5;
  typedef struct { bit [3:0] lo = C; bit [3:0] hi; int n = -3; real r = 1.5; } s_t;
  typedef struct { s_t in; byte b = 8'h7f; } outer_t;
  s_t m;
  outer_t o;
  class k; s_t p; endclass
  initial begin
    automatic s_t a;
    k h = new;
    $display("%h %h %0d %0.1f", m.lo, m.hi, m.n, m.r);
    $display("%h %0d %h", o.in.lo, o.in.n, o.b);
    $display("%h %0d", a.lo, a.n);
    $display("%h %0d", h.p.lo, h.p.n);
  end
endmodule
//> 5 0 -3 1.5
//> 5 -3 7f
//> 5 -3
//> 5 -3
//! icarus-skip: Icarus has no unpacked structs
