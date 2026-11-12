// IEEE 1800-2023 37.3, 37.5, 37.10 to 37.25: the SystemVerilog part of the
// VPI object model. Packages, an interface with modports and a program
// are instances; generate scopes, tasks, functions and named blocks are
// internal scopes; variables report their SystemVerilog type (vpiBitVar,
// vpiIntVar, vpiEnumVar, vpiStructVar, vpiStringVar, ...) and a typespec
// (enum constants, struct members, typedef names); arrays have a range per
// dimension and elements by all indices; a port has vpiHighConn.
//! icarus-skip: Icarus 13 does not model SystemVerilog objects in VPI
//! args: data/vpi/svobjects.c
package pk;
  typedef enum logic [1:0] {RED, GREEN = 2, BLUE} color_t;
  typedef struct packed { logic [3:0] hi; bit [3:0] lo; } pair_t;
  parameter int PW = 5;
  int counter = 3;
endpackage
interface bus_if (input logic clk);
  logic [7:0] data;
  logic valid;
  modport src (output data, valid);
  modport dst (input data, valid);
endinterface
module leaf (input [3:0] a, output [3:0] y, input [7:0] z);
  assign y = ~a;
endmodule
program pg (input logic clk);
  int seen = 4;
endprogram
module top;
  import pk::*;
  logic clk = 0;
  bit [3:0] b = 4'd5;
  int i = -7;
  shortint si = 12;
  longint li = 64'd1 << 40;
  byte by = 8'h41;
  string s = "hello";
  real r = 1.5;
  color_t col = BLUE;
  pair_t p = 8'ha5;
  logic [7:0] m2 [0:1][2:0];
  wire [3:0] w;
  bus_if bi (clk);
  pg pi (clk);
  leaf u (.a(b), .y(w), .z(m2[1][2]));
  for (genvar g = 0; g < 2; g++) begin : gen
    logic [1:0] gv = 2'(g + 1);
  end
  if (PW == 5) begin : cond
    logic c1 = 1;
  end
  function automatic int twice(int x);
    return 2 * x;
  endfunction
  task show;
  endtask
  initial begin : blk
    m2[1][2] = 8'h3c;
    m2[0][0] = 8'h11;
    #1 $sv_walk;
  end
endmodule
//> vpiModule top (top)
//>   vpiReg clk size 1 = 0 (vpiLogicTypespec)
//>   vpiBitVar b[3:0] size 4 = 5 (vpiBitTypespec)
//>   vpiIntVar i size 32 = -7 (vpiIntTypespec)
//>   vpiShortIntVar si size 16 = 12 (vpiShortIntTypespec)
//>   vpiLongIntVar li size 64 = 0000010000000000 (vpiLongIntTypespec)
//>   vpiByteVar by size 8 = 41 (vpiByteTypespec)
//>   vpiStringVar s size 40 = hello (vpiStringTypespec)
//>   vpiRealVar r size 64 = 1.5 (vpiRealTypespec)
//>   vpiEnumVar col size 2 = 3 (vpiEnumTypespec)
//>   vpiStructVar p size 8 = a5 (vpiStructTypespec)
//>   vpiRegArray m2[0:1][2:0] size 6 array
//>   vpiNet w[3:0] size 4 = a (vpiLogicTypespec)
//>   vpiGenScopeArray gen size 2
//>   vpiGenScope gen[0] (top.gen[0])
//>     parameter g = 0
//>     vpiReg gv[1:0] size 2 = 1 (vpiLogicTypespec)
//>   vpiGenScope gen[1] (top.gen[1])
//>     parameter g = 1
//>     vpiReg gv[1:0] size 2 = 2 (vpiLogicTypespec)
//>   vpiGenScope cond (top.cond)
//>     vpiReg c1 size 1 = 1 (vpiLogicTypespec)
//>   vpiFunction twice (top.twice)
//>     vpiIntVar twice size 32 = 0 (vpiIntTypespec)
//>   vpiTask show (top.show)
//>   vpiNamedBegin blk (top.blk)
//>   vpiInterface bi (top.bi)
//>     port clk low top.bi.clk high top.clk
//>     modport src
//>     modport dst
//>     vpiReg data[7:0] size 8 = xx (vpiLogicTypespec)
//>     vpiReg valid size 1 = x (vpiLogicTypespec)
//>     vpiNet clk size 1 = 0 (vpiLogicTypespec)
//>   vpiProgram pi (top.pi)
//>     port clk low top.pi.clk high top.clk
//>     vpiIntVar seen size 32 = 4 (vpiIntTypespec)
//>     vpiNet clk size 1 = 0 (vpiLogicTypespec)
//>   vpiModule u (top.u)
//>     port a low top.u.a high top.b
//>     port y low top.u.y high top.w
//>     port z low top.u.z high top.m2[1][2]
//>     vpiNet a[3:0] size 4 = 5 (vpiLogicTypespec)
//>     vpiNet y[3:0] size 4 = a (vpiLogicTypespec)
//>     vpiNet z[7:0] size 8 = 3c (vpiLogicTypespec)
//> vpiPackage pk (pk)
//>   parameter PW = 5
//>   typespec vpiEnumTypespec color_t size 2
//>     RED = 0
//>     GREEN = 2
//>     BLUE = 3
//>   typespec vpiStructTypespec pair_t size 8
//>     member hi: vpiLogicTypespec
//>     member lo: vpiBitTypespec
//>   vpiIntVar counter size 32 = 3 (vpiIntTypespec)
//> top.m2[1][2] is a vpiReg = 3c, same as by name: 1
//> top.m2[1][2][3] = 1
//> m2 has 6 elements
//> pk::counter in a vpiPackage = 3
//> top.gen[1].gv in a vpiGenScope, module top
//> twice returns vpiIntTypespec, automatic 1, vpiIntFunc 1
//> col = 2
