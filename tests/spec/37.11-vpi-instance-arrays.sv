// IEEE 1800-2023 37.11 (instance arrays) and 37.5: vpi_iterate(vpiModuleArray /
// vpiInterfaceArray / vpiPrimitiveArray / vpiInstanceArray, module) gives
// a module's instance arrays. An array has vpiName, vpiFullName, vpiSize
// (all its elements), vpiLeftRange / vpiRightRange (the leftmost
// dimension), vpi_iterate(vpiRange) (leftmost first, detail 2), its
// elements (vpiModule, vpiInterface, vpiPrimitive, vpiInstance),
// vpi_handle_by_index and vpi_handle_by_multi_index. From an element,
// vpi_handle(vpiIndex) gives its index and vpi_handle(vpiModuleArray) its
// array (NULL outside an array). The elements are named "u[2]" (23.3.3).
//! icarus-skip: Icarus 13 has no interface arrays or vpiInstanceArray
//! args: data/vpi/instarr.c
module leaf(input logic i);
endmodule
interface bus;
  logic v;
endinterface
module top;
  logic [3:0] a = 4'b1010, y;
  leaf u[3:1](a[2:0]);
  leaf m[0:1][2:0](1'b0);
  leaf solo(a[0]);
  bus b[2]();
  and g[3:0](y, a, 4'b1111);
  initial #1 $arrays;
endmodule
//> vpiModuleArray top.u name u size 3 range 3:1 ranges [3:1]: u[3](3) u[2](2) u[1](1)
//> vpiModuleArray top.m name m size 6 range 0:1 ranges [0:1] [2:0]: m[0][2](2) m[0][1](1) m[0][0](0) m[1][2](2) m[1][1](1) m[1][0](0)
//> vpiInterfaceArray top.b name b size 2 range 0:1 ranges [0:1]: b[0](0) b[1](1)
//> vpiGateArray top.g name g size 4 range 3:0 ranges [3:0]: g[3](3) g[2](2) g[1](1) g[0](0)
//> modules: u m
//> primitives: g[3] g[2] g[1] g[0]
//> u[2]: top.u[2] array u
//> m[1][0]: top.m[1][0] array m
//> solo: index none array none
//> by name: u
