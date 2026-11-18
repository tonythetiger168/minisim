//! icarus-skip: Icarus has no unpacked structs
// IEEE 1800-2023 11.4.11: the conditional operator on unpacked structures
// and unions. When an arm calls a function, only the chosen arm is
// evaluated, and the result is the whole structure.
package pk;
  typedef struct { logic [2:0] sel; bit hi; byte tag; } cfg_t;
  int calls;
  function automatic cfg_t make_hi;
    cfg_t c, c2; c.sel = 3'd5; c.hi = 1; c.tag = "H"; calls++; return c;
  endfunction
  function automatic cfg_t make_lo();
    cfg_t c, c2; c.sel = 3'd2; c.hi = 0; c.tag = "L"; calls += 10; return c;
  endfunction
endpackage
module top;
  import pk::*;
  typedef union { int i; shortint s; } u_t;
  function automatic u_t mk_u(int v); u_t u; u.i = v; return u; endfunction
  logic up;
  cfg_t c, c2;
  u_t u;
  always @(up) c = up ? make_hi() : make_lo();
  initial begin
    up = 1; #1 $display("%0d %0d %s", c.sel, c.hi, c.tag);
    up = 0; #1 $display("%0d %0d %s", c.sel, c.hi, c.tag);
    calls = 0;
    c2 = up ? make_hi() : make_lo();
    $display("%s calls=%0d", c2.tag, calls);
    u = up ? mk_u(7) : mk_u(-3);
    $display("%0d", u.i);
  end
endmodule
//> 5 1 H
//> 2 0 L
//> L calls=10
//> -3
