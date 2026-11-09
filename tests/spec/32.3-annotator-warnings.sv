`timescale 1ns/1ns
// IEEE 1800-2023 32.3, 32.9: the SDF annotator warns about any data it
// cannot annotate, ignores constructs unrelated to SystemVerilog timing
// (TIMINGENV) without a warning, and leaves a value that the file does not
// give unchanged. The log file (the fourth argument; here standard output)
// has one entry per annotation and each warning.
//! icarus-skip: Icarus 13 does not back-annotate specify paths
module bufp(input A, output Z);
  assign Z = A;
  specify
    (A => Z) = 1;
  endspecify
endmodule
module top;
  reg a = 0;
  wire z1, z2;
  bufp u1(a, z1);
  bufp u2(a, z2);   // nothing in the file reaches it: its delay stays 1
  initial begin
    $sdf_annotate("data/sdf/32.3-annotator-warnings.sdf", top, , "/dev/stdout");
    #10 a = 1;
    #10 $finish;
  end
  always @(z1) if ($time > 5) $display("%0t z1=%b", $time, z1);
  always @(z2) if ($time > 5) $display("%0t z2=%b", $time, z2);
endmodule

//> SDF data/sdf/32.3-annotator-warnings.sdf -> top (TYPICAL)
//> data/sdf/32.3-annotator-warnings.sdf:5: IOPATH A Z -> top.u1 (1 path)
//> Warning: data/sdf/32.3-annotator-warnings.sdf:6: no specify path top.u1 (B => Z) for IOPATH B Z
//> Warning: data/sdf/32.3-annotator-warnings.sdf:8: no instance top.u9
//> Warning: data/sdf/32.3-annotator-warnings.sdf:10: instance top.u2 is a bufp, not the CELLTYPE inv
//> Warning: data/sdf/32.3-annotator-warnings.sdf:13: no timing check in top.u1 for SETUP A Z
//> 1 annotations, 4 warnings
//> 11 z2=1
//> 13 z1=1
