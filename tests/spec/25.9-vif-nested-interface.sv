// IEEE 1800-2023 25.9: an interface instance declared inside another
// interface is reached through a virtual interface of the outer one:
// v.sub.x reads and writes its variables, v.sub.f() calls its methods,
// @(v.sub.x) waits on it, and v.sub is itself a virtual interface value.
// Each outer instance has its own inner instances.
//! icarus-skip: Icarus 13 does not support virtual interfaces
interface counter_if;
  logic [7:0] cnt = 0;
  function automatic void bump(); cnt = cnt + 1; endfunction
  task automatic add(input logic [7:0] n); cnt = cnt + n; endtask
  function automatic logic [7:0] get(); return cnt; endfunction
endinterface
interface pair_if;
  counter_if a ();
  counter_if b ();
endinterface
interface deep_if;
  pair_if p ();
endinterface
class Driver;
  virtual pair_if vif;
  function new(virtual pair_if v); vif = v; endfunction
  task run();
    vif.a.bump();
    vif.b.add(5);
    vif.b.cnt = vif.b.cnt + 1;
  endtask
  function logic [7:0] sum(); return vif.a.get() + vif.b.cnt; endfunction
endclass
module top;
  pair_if p1 (), p2 ();
  deep_if d ();
  virtual deep_if dv = d;
  virtual counter_if one;
  Driver drv;
  initial begin
    drv = new(p2);
    drv.run();
    $display("%0d %0d %0d %0d sum=%0d", p1.a.cnt, p1.b.cnt, p2.a.cnt, p2.b.cnt, drv.sum());
    one = drv.vif.b;
    one.bump();
    fork begin @(dv.p.a.cnt); $display("deep %0d at %0t", d.p.a.cnt, $time); end join_none
    #1 dv.p.a.bump();
    #1 $display("%0d %0d", p2.b.cnt, d.p.a.cnt);
  end
endmodule
//> 0 0 1 6 sum=7
//> deep 1 at 1
//> 7 1
