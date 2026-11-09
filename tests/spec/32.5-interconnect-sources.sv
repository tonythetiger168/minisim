`timescale 1ns/1ns
// IEEE 1800-2023 32.4.4, 32.5: on a net with several sources, INTERCONNECT
// delays one source-load pair. After PORT l1.in (6), INTERCONNECT s3.o
// l1.in (5) changes only the delay from s3: the others stay 6. A source
// deeper in the hierarchy (w.u.o, through w's output port) gets its own
// delay too. A PORT after an INTERCONNECT replaces it: every source to l2
// takes 6. Each source drives the net alone in turn.
//! icarus-skip: Icarus 13 does not annotate interconnect delays
module src(input en, i, output o);
  assign o = en ? i : 1'bz;
endmodule
module wrap(input en, i, output o);
  src u(en, i, o);
endmodule
module ld(input in);
  always @(in) if ($time > 0) $display("%0t: %m.in = %b", $time, in);
endmodule
module top;
  reg e1 = 0, e2 = 0, e3 = 0, e4 = 0, i = 1;
  wire n;
  src s1(e1, i, n);
  src s2(e2, i, n);
  src s3(e3, i, n);
  wrap w(e4, i, n);
  ld l1(n);
  ld l2(n);
  initial begin
    $sdf_annotate("data/sdf/32.5-interconnect-sources.sdf", top);
    #10 e1 = 1; #10 e1 = 0;
    #10 e2 = 1; #10 e2 = 0;
    #10 e3 = 1; #10 e3 = 0;
    #10 e4 = 1; #10 e4 = 0;
    #10 $finish;
  end
endmodule
//> 16: top.l1.in = 1
//> 16: top.l2.in = 1
//> 26: top.l1.in = z
//> 26: top.l2.in = z
//> 36: top.l1.in = 1
//> 36: top.l2.in = 1
//> 46: top.l1.in = z
//> 46: top.l2.in = z
//> 55: top.l1.in = 1
//> 56: top.l2.in = 1
//> 65: top.l1.in = z
//> 66: top.l2.in = z
//> 72: top.l1.in = 1
//> 76: top.l2.in = 1
//> 82: top.l1.in = z
//> 86: top.l2.in = z
