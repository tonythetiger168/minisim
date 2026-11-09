`timescale 1ns/100ps
// IEEE 1800-2023 32.5, 32.6: annotations apply in order. INCREMENT adds to
// the value so far and ABSOLUTE replaces it; a later $sdf_annotate (with
// its own scope, the second argument) builds on the earlier ones. A delay
// below 0 after INCREMENT is 0. "((3.5) () ())" annotates the delay alone.
//! icarus-skip: Icarus 13 does not back-annotate specify paths
module bufp(input A, output Z);
  assign Z = A;
  specify
    (A => Z) = 1;
  endspecify
endmodule
module top;
  reg a = 0;
  wire z1, z2, z3;
  bufp u1(a, z1);   // ABSOLUTE (2) (3), then INCREMENT (1): 3 and 4
  bufp u2(a, z2);   // ((4) () ()), then the second file adds 0.5: 4.5
  bufp u3(a, z3);   // INCREMENT (-5): 0
  initial begin
    $sdf_annotate("data/sdf/32.5-first.sdf", top);
    $sdf_annotate("data/sdf/32.5-second.sdf", top.u2);
    #10 a = 1;
    #10 a = 0;
    #10 $finish;
  end
  always @(z1) if ($time > 5) $display("%.1f z1=%b", $realtime, z1);
  always @(z2) if ($time > 5) $display("%.1f z2=%b", $realtime, z2);
  always @(z3) if ($time > 5) $display("%.1f z3=%b", $realtime, z3);
endmodule

//> 10.0 z3=1
//> 13.0 z1=1
//> 14.5 z2=1
//> 20.0 z3=0
//> 24.0 z1=0
//> 24.5 z2=0
