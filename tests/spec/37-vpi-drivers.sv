// IEEE 1800-2023 37.16 (nets) and 37.17 (variables): vpiLocalDriver and
// vpiLocalLoad give the continuous assignments and instance ports inside
// the module of a net or variable that drive or read it; vpiDriver and
// vpiLoad follow whole-signal port connections through the hierarchy;
// vpiPortInst gives the ports of the instances a net connects to.
//! icarus-skip: Icarus 13 does not give continuous assignments as drivers
//! args: data/vpi/drivers.c
module inv(input logic i, output logic o);
  assign o = ~i;
endmodule
module top;
  logic a = 0;
  wire n, y;
  logic [1:0] v;
  assign n = a;
  inv u(.i(n), .o(y));
  assign v = {y, n};
  initial begin
    #1 $drivers;
    $display("y %b v %b", y, v);
  end
endmodule
//> top.a local drivers:
//> top.a local loads: vpiContAssign@15
//> top.a drivers:
//> top.a loads: vpiContAssign@15
//> top.a instance ports:
//> top.n local drivers: vpiContAssign@15
//> top.n local loads: vpiPort(top.u.i dir 1) vpiContAssign@17
//> top.n drivers: vpiContAssign@15
//> top.n loads: vpiContAssign@17 vpiContAssign@9
//> top.n instance ports: vpiPort(top.u.i dir 1)
//> top.y local drivers: vpiPort(top.u.o dir 2)
//> top.y local loads: vpiContAssign@17
//> top.y drivers: vpiContAssign@9
//> top.y loads: vpiContAssign@17
//> top.y instance ports: vpiPort(top.u.o dir 2)
//> top.v local drivers: vpiContAssign@17
//> top.v local loads:
//> top.v drivers: vpiContAssign@17
//> top.v loads:
//> top.v instance ports:
//> top.u.i local drivers:
//> top.u.i local loads: vpiContAssign@9
//> top.u.i drivers: vpiContAssign@15
//> top.u.i loads: vpiContAssign@9 vpiContAssign@17
//> top.u.i instance ports:
//> top.u.o local drivers: vpiContAssign@9
//> top.u.o local loads:
//> top.u.o drivers: vpiContAssign@9
//> top.u.o loads: vpiContAssign@17
//> top.u.o instance ports:
//> y 1 v 10
