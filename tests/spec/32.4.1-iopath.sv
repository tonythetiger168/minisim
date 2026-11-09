`timescale 1ns/100ps
// IEEE 1800-2023 32.4.1: SDF IOPATH delays annotate specify paths with the
// same ports. A COND IOPATH annotates only the path with that condition,
// one without COND annotates every path between the two ports, and
// CONDELSE annotates the ifnone path. DEVICE annotates every path to the
// outputs; INSTANCE * applies a CELL to every instance of its CELLTYPE.
//! icarus-skip: Icarus 13 does not back-annotate specify paths
module plain(input sel, mode, output zout);
  assign zout = sel;
  specify
    (sel => zout) = 0;
  endspecify
endmodule
module twocond(input sel, mode, output zout);
  assign zout = sel;
  specify
    if (!mode) (sel => zout) = 0;
    if (mode) (sel => zout) = 0;
  endspecify
endmodule
module withnone(input sel, mode, output zout);
  assign zout = sel;
  specify
    if (mode) (sel => zout) = 0;
    ifnone (sel => zout) = 0;
  endspecify
endmodule
module dev(input sel, mode, output zout);
  assign zout = sel;
  specify
    (sel => zout) = 1;
  endspecify
endmodule
module top;
  reg sel = 0, mode = 0;
  wire z1, z2, z3, z4, z5;
  plain u1(sel, mode, z1);
  twocond u2(sel, mode, z2);   // COND mode: only the second path
  twocond u3(sel, mode, z3);   // no COND: both paths
  withnone u4(sel, mode, z4);  // CONDELSE: the ifnone path
  dev u5(sel, mode, z5);       // DEVICE through INSTANCE *
  initial begin
    $sdf_annotate("data/sdf/32.4.1-iopath.sdf", top);
    #10 sel = 1;
    #10 sel = 0;
    #5 mode = 1;
    #5 sel = 1;
    #10 sel = 0;
    #10 $finish;
  end
  always @(z1) if ($time > 5) $display("%.1f z1=%b", $realtime, z1);
  always @(z2) if ($time > 5) $display("%.1f z2=%b", $realtime, z2);
  always @(z3) if ($time > 5) $display("%.1f z3=%b", $realtime, z3);
  always @(z4) if ($time > 5) $display("%.1f z4=%b", $realtime, z4);
  always @(z5) if ($time > 5) $display("%.1f z5=%b", $realtime, z5);
endmodule

//> 10.0 z2=1
//> 11.1 z3=1
//> 11.3 z1=1
//> 12.0 z4=1
//> 14.0 z5=1
//> 20.0 z2=0
//> 21.7 z1=0
//> 21.9 z3=0
//> 23.0 z4=0
//> 24.0 z5=0
//> 30.0 z4=1
//> 31.1 z3=1
//> 31.2 z2=1
//> 31.3 z1=1
//> 34.0 z5=1
//> 40.0 z4=0
//> 41.6 z2=0
//> 41.7 z1=0
//> 41.9 z3=0
//> 44.0 z5=0
