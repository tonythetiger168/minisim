// IEEE 1800-2023 25.6: interfaces and specify blocks. Each signal of an
// interface port is a terminal: a module path to an interface output
// delays the module's writes to it, and timing checks see its inputs.
// The example of 25.6, with a setup violation added.
//! icarus-skip: Icarus 13 has no interface ports in specify blocks
`timescale 1ns/100ps
interface itf;
  logic c, q, d;
  modport flop (input c, d, output q);
endinterface
module dtype (itf.flop ch);
  always_ff @(posedge ch.c) ch.q <= ch.d;
  specify
    (posedge ch.c => (ch.q +: ch.d)) = (5, 6);
    $setup(ch.d, posedge ch.c, 1);
  endspecify
endmodule
module top;
  itf i();
  dtype u(i.flop);
  initial begin
    i.c = 0; i.d = 1;
    #10 i.c = 1;
    #10 i.c = 0; i.d = 0;
    #10 i.c = 1;
    #10 i.c = 0;
    #9.5 i.d = 1;
    #0.5 i.c = 1;
    #20 $finish;
  end
  always @(i.q) $display("%0t i.q=%b", $time, i.q);
endmodule
//> 150 i.q=1
//> 360 i.q=0
//> "25.6-interface-specify.sv", 15: Timing violation in top.u
//>     $setup( ch.d:49.5, posedge ch.c:50, 1 );
//> 550 i.q=1
