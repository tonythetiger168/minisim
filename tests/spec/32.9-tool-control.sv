`timescale 1ns/100ps
// IEEE 1800-2023 32.9 (TOOL_CONTROL), 11.11: msim +maxdelays picks the max of
// every min:typ:max, in the source and in an SDF file whose mtm_spec is
// TOOL_CONTROL (the default). msim -sdf [min|typ|max:]instance:file
// annotates like a $sdf_annotate before time 0; its min here wins over
// +maxdelays for that file.
//! icarus-skip: Icarus 13 does not back-annotate specify paths
//! args: +maxdelays -sdf min:top.u3:data/sdf/32.9-mtm.sdf
module bufp(input A, output Z);
  assign Z = A;
  specify
    (A => Z) = (4:5:6);
  endspecify
endmodule
module top;
  reg a = 0;
  wire z1, z2, z3;
  bufp u1(a, z1);   // no SDF: (4:5:6) is 6
  bufp u2(a, z2);   // TOOL_CONTROL: (1:2:3) is 3
  bufp u3(a, z3);   // -sdf min: 1
  initial begin
    $sdf_annotate("data/sdf/32.9-mtm.sdf", top.u2);
    #10 a = 1;
    #10;
    #(1:2:3) $display("%.1f #(1:2:3)", $realtime);   // 3
    $finish;
  end
  always @(z1) if ($time >= 10) $display("%.1f u1", $realtime);
  always @(z2) if ($time >= 10) $display("%.1f u2", $realtime);
  always @(z3) if ($time >= 10) $display("%.1f u3", $realtime);
endmodule

//> 11.0 u3
//> 13.0 u2
//> 16.0 u1
//> 23.0 #(1:2:3)
