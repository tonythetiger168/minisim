// IEEE 1800-2023 37.28 (parameter, def param, param assign), 37.11 detail 1
// and 37.40: vpi_iterate(vpiParameter, module) gives the value and the
// type parameters; vpiExpr of a parameter is its default (detail 3);
// vpi_iterate(vpiParamAssign, module) gives the overrides of #(...) with
// vpiLhs the parameter (detail 4), vpiRhs the override and vpiConnByName;
// vpi_iterate(vpiDefParam, module) the defparams; vpiExpr of an instance
// array is a vpiListOp operation of its connections; a net declaration
// assignment is a vpiContAssign with vpiNetDeclAssign.
//! icarus-skip: Icarus 13 has no type parameters or param assigns through VPI
//! args: data/vpi/paramasg.c
module leaf #(parameter int W = 2 + 2, parameter type T = logic [3:0], parameter D = 1) (input logic [W-1:0] i);
  localparam int L = W * 2;
  T t;
endmodule
module top;
  logic [7:0] a = 8'h5a;
  wire [7:0] w = a + 1;
  leaf #(.W(8), .T(byte)) u1(a);
  leaf #(3) u2(a[2:0]);
  leaf u3[1:0](a[7:0]);
  defparam u2.D = 7;
  initial #1 $params;
endmodule
//> u1: W=8 (default 4, local 0) T=vpiTypeParameter D=1 (default 1, local 0) L=16 (default 16, local 1)
//>   assign W = 8 by name 1
//>   assign T -> typespec by name 1
//> u2: W=3 (default 4, local 0) T=vpiTypeParameter D=7 (default 1, local 0) L=6 (default 6, local 1)
//>   assign W = 3 by name 0
//> top defparam D = 7
//> u3 connections: vpiOperation op 37: a[7:0]
//> w: net decl 1 lhs w rhs 91
