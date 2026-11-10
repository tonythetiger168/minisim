`timescale 1ns/100ps
// IEEE 1800-2023 32.9: $sdf_annotate(sdf_file, module_instance, config_file,
// log_file, mtm_spec, scale_factors, scale_type). mtm_spec picks the min,
// typ or max of each triple (TOOL_CONTROL, the default, is typ unless
// msim +mindelays / +maxdelays); scale_factors multiply the min, typ and
// max values, and scale_type says which value they are taken from. The
// config file's MTM_SPEC and SCALE_FACTORS apply unless the arguments say
// otherwise. With no module_instance, the scope is the module that calls
// $sdf_annotate. The file name may be a string variable.
//! icarus-skip: Icarus 13 does not back-annotate specify paths
module bufp(input A, output Z);
  assign Z = A;
  specify
    (A => Z) = 1;
  endspecify
endmodule
module wrapper(input A, output Z);
  bufp b(A, Z);
  initial $sdf_annotate("data/sdf/32.9-wrapped.sdf");   // scope: this instance
endmodule
module top;
  reg a = 0;
  wire z1, z2, z3, z4, z5, z6, z7, z8;
  string f = "data/sdf/32.9-other.sdf";
  bufp u1(a, z1);      // MINIMUM: 1
  bufp u2(a, z2);      // MAXIMUM: 3
  bufp u3(a, z3);      // TOOL_CONTROL: typ, 2
  bufp u4(a, z4);      // TYPICAL, 1.6:1.4:1.2: 2 * 1.4 = 2.8
  bufp u5(a, z5);      // MAXIMUM, 1.6:1.4:1.2, FROM_MINIMUM: 1 * 1.2 = 1.2
  bufp u6(a, z6);      // config file: MAXIMUM, 1:1:0.5: 1.5
  bufp u7(a, z7);      // a file named by a string variable (0.5:0.7:0.9): 0.7
  wrapper u8(a, z8);   // annotated from inside the wrapper: 2.5
  initial begin
    $sdf_annotate("data/sdf/32.9-mtm.sdf", top.u1, , , "MINIMUM");
    $sdf_annotate("data/sdf/32.9-mtm.sdf", top.u2, , , "MAXIMUM");
    $sdf_annotate("data/sdf/32.9-mtm.sdf", top.u3);
    $sdf_annotate("data/sdf/32.9-mtm.sdf", top.u4, , , "TYPICAL", "1.6:1.4:1.2");
    $sdf_annotate("data/sdf/32.9-mtm.sdf", top.u5, , , "MAXIMUM", "1.6:1.4:1.2", "FROM_MINIMUM");
    $sdf_annotate("data/sdf/32.9-mtm.sdf", top.u6, "data/sdf/32.9-mtm.cfg");
    $sdf_annotate(f, top.u7);
    #10 a = 1;
    #10 $finish;
  end
  always @(z1) if ($time > 5) $display("%.1f u1", $realtime);
  always @(z2) if ($time > 5) $display("%.1f u2", $realtime);
  always @(z3) if ($time > 5) $display("%.1f u3", $realtime);
  always @(z4) if ($time > 5) $display("%.1f u4", $realtime);
  always @(z5) if ($time > 5) $display("%.1f u5", $realtime);
  always @(z6) if ($time > 5) $display("%.1f u6", $realtime);
  always @(z7) if ($time > 5) $display("%.1f u7", $realtime);
  always @(z8) if ($time > 5) $display("%.1f u8", $realtime);
endmodule

//> 10.7 u7
//> 11.0 u1
//> 11.2 u5
//> 11.5 u6
//> 12.0 u3
//> 12.5 u8
//> 12.8 u4
//> 13.0 u2
