// IEEE 1800-2023 6.6.7: the data type of a user-defined nettype may be a
// fixed-size array of reals, or a struct holding one; the resolution function
// gets every driver's whole value.
//! icarus-skip: Icarus 13 does not support nettype
//> 1.500000 2.500000
//> 1.500000 2.500000 2
typedef real pair_t[2];
typedef struct { real v[2]; int c; } acc_t;
function automatic pair_t addp(input pair_t d[]);
  addp = '{0.0, 0.0};
  foreach (d[i]) begin addp[0] += d[i][0]; addp[1] += d[i][1]; end
endfunction
function automatic acc_t adda(input acc_t d[]);
  adda.v = '{0.0, 0.0};
  adda.c = d.size();
  foreach (d[i]) begin adda.v[0] += d[i].v[0]; adda.v[1] += d[i].v[1]; end
endfunction
nettype pair_t pnet with addp;
nettype acc_t anet with adda;
module top;
  pnet p;
  anet a;
  assign p = '{1.0, 2.0};
  assign p = '{0.5, 0.5};
  assign a = '{'{1.0, 2.0}, 0};
  assign a = '{'{0.5, 0.5}, 0};
  initial #1 begin
    $display("%f %f", p[0], p[1]);
    $display("%f %f %0d", a.v[0], a.v[1], a.c);
  end
endmodule
