// 6.6.7, 23.3.3.7.2: user-defined nettype ports. A port whose two sides are nets
// of one user-defined nettype is one simulated net, so every driver on either
// side goes to the resolution function.
//! icarus-skip: Icarus 13 does not support nettype
//> bus v=2.750000 cnt=3
//> other v=4.000000 cnt=1
//> mid.i v=2.750000 cnt=3
package p;
  typedef struct { real v; int cnt; } pt;
  function automatic pt add(input pt d[]);
    add.v = 0.0; add.cnt = d.size();
    foreach (d[i]) add.v += d[i].v;
  endfunction
  nettype pt pn with add;
  nettype pt pn2 with add;   // same type and resolver
endpackage
import p::*;

module src #(parameter real V = 1.0) (output pn o);
  assign o = '{V, 0};
endmodule

module mid (inout pn io, input pn i);
  src #(0.5) s(io);
  assign io = '{0.25, 0};
  initial #2 $display("mid.i v=%f cnt=%0d", i.v, i.cnt);
endmodule

module top;
  pn bus;
  pn2 other;
  src #(2.0) a(bus);
  mid m(.io(bus), .i(bus));
  src #(4.0) b(other);
  initial begin
    #1 $display("bus v=%f cnt=%0d", bus.v, bus.cnt);
    $display("other v=%f cnt=%0d", other.v, other.cnt);
  end
endmodule
