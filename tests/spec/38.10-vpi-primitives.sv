// IEEE 1800-2023 37 (primitive objects) and 38.10: vpi_iterate(vpiPrimitive,
// module) gives the gates, switches and UDP instances of a module
// (vpiGate / vpiSwitch / vpiUdp also select one class), vpiPrimType and
// vpiDefName name the primitive, vpiPrimTerm the terminals (vpiTermIndex,
// vpiDirection, vpiExpr: the connected signal); vpi_get_delays reads a
// gate's rise, fall and turn-off delays and vpi_put_delays changes them:
// y then follows a with rise 5, fall 7.
//! icarus-skip: Icarus 13 has no primitive delays through VPI
//! args: data/vpi/prims.c
`timescale 1ns/1ns
primitive inv(output o, input i);
  table
    0 : 1;
    1 : 0;
  endtable
endprimitive
module top;
  reg a = 0, en = 1;
  wire y, n, s, t1, t2;
  and #(2, 3) g1(y, a, en);
  not g2(n, a);
  bufif1 #(1, 1, 4) g3(s, a, en);
  inv u1(t1, a);
  tran sw(t1, t2);
  initial begin
    #10 a = 1;
    #10 a = 0;
    #10 $prims;
    a = 1;
    #10 a = 0;
    #10 $finish;
  end
  always @(y) $display("%0t y %b", $time, y);
endmodule
//> 3 y 0
//> 12 y 1
//> 23 y 0
//> vpiGate top.g1 def and prim 1 size 3: [0 dir 2 y] [1 dir 1 a] [2 dir 1 en] delays 2 3 2
//> vpiGate top.g2 def not prim 8 size 2: [0 dir 2 n] [1 dir 1 a] delays 0 0 0
//> vpiGate top.g3 def bufif1 prim 10 size 3: [0 dir 2 s] [1 dir 1 a] [2 dir 1 en] delays 1 1 4
//> vpiUdp top.u1 def inv prim 28 size 2: [0 dir 2 t1] [1 dir 1 a]
//> vpiSwitch top.sw def tran prim 22 size 2: [0 dir 3 t1] [1 dir 3 t2] delays 0 0 0
//> switches: sw
//> g1 now: delays 5 7 5
//> 35 y 1
//> 47 y 0
