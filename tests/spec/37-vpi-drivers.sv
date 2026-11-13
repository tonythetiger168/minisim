// IEEE 1800-2023 37.16 (nets) and 37.17 (variables): vpiLocalDriver and
// vpiLocalLoad give the continuous assignments and instance ports inside
// the module of a net or variable that drive or read it; vpiDriver and
// vpiLoad follow whole-signal port connections through the hierarchy;
// vpiPortInst gives the ports of the instances a net connects to. The
// ports of a net's own module are local drivers (input, inout) and loads
// (output, inout) of it (37.16 detail 17). On a bit, only the drivers and
// loads that touch that bit count (detail 18). Primitive terminals drive
// (outputs) and load (inputs) the nets they connect to (37.46).
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
  wire [3:0] w;
  wire g_out;
  assign w[1:0] = {a, a};
  assign w[3:2] = 2'b10;
  and g1(g_out, w[3], n);
  initial begin
    #1 $drivers;
    $display("y %b v %b", y, v);
  end
endmodule
//> top.a local drivers:
//> top.a local loads: vpiContAssign@19 vpiContAssign@24
//> top.a drivers:
//> top.a loads: vpiContAssign@19 vpiContAssign@24
//> top.a instance ports:
//> top.n local drivers: vpiContAssign@19
//> top.n local loads: vpiPort(top.u.i dir 1) vpiContAssign@21 vpiPrimTerm(g1 term 2)
//> top.n drivers: vpiContAssign@19
//> top.n loads: vpiContAssign@21 vpiPrimTerm(g1 term 2) vpiContAssign@13
//> top.n instance ports: vpiPort(top.u.i dir 1)
//> top.y local drivers: vpiPort(top.u.o dir 2)
//> top.y local loads: vpiContAssign@21
//> top.y drivers: vpiContAssign@13
//> top.y loads: vpiContAssign@21
//> top.y instance ports: vpiPort(top.u.o dir 2)
//> top.v local drivers: vpiContAssign@21
//> top.v local loads:
//> top.v drivers: vpiContAssign@21
//> top.v loads:
//> top.v instance ports:
//> top.u.i local drivers: vpiPort(top.u.i dir 1)
//> top.u.i local loads: vpiContAssign@13
//> top.u.i drivers: vpiContAssign@19
//> top.u.i loads: vpiContAssign@13 vpiContAssign@21 vpiPrimTerm(g1 term 2)
//> top.u.i instance ports:
//> top.u.o local drivers: vpiContAssign@13
//> top.u.o local loads: vpiPort(top.u.o dir 2)
//> top.u.o drivers: vpiContAssign@13
//> top.u.o loads: vpiContAssign@21
//> top.u.o instance ports:
//> top.w local drivers: vpiContAssign@24 vpiContAssign@25
//> top.w local loads: vpiPrimTerm(g1 term 1)
//> top.w drivers: vpiContAssign@24 vpiContAssign@25
//> top.w loads: vpiPrimTerm(g1 term 1)
//> top.w instance ports:
//> top.g_out local drivers: vpiPrimTerm(g1 term 0)
//> top.g_out local loads:
//> top.g_out drivers: vpiPrimTerm(g1 term 0)
//> top.g_out loads:
//> top.g_out instance ports:
//> top.w[0] local drivers: vpiContAssign@24
//> top.w[0] local loads:
//> top.w[2] local drivers: vpiContAssign@25
//> top.w[2] local loads:
//> top.w[3] local drivers: vpiContAssign@25
//> top.w[3] local loads: vpiPrimTerm(g1 term 1)
//> y 1 v 10
