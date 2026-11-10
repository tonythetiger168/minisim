`timescale 1ns/1ns
// IEEE 1800-2023 32.4.1, Table 32-1: DEVICE annotates every specify path
// to the module's outputs, or with no such path every primitive driving
// them. DEVICE y names one output (u1); with no port, all of them (u2); a
// module with a path gets it on the path, not on its gate (u3). From the
// parent, DEVICE u4.z names an instance's output and DEVICE u5 the
// instance; INCREMENT adds to an annotated delay (u5).
//! icarus-skip: Icarus 13 does not annotate SDF
module cellg(input a, b, output y, z);
  and #1 g1(y, a, b);
  not g2(z, a);
endmodule
module cellp(input a, output y);
  buf g(y, a);
  specify
    (a => y) = 2;
  endspecify
endmodule
module top;
  reg a = 0, b = 0;
  wire y1, z1, y2, z2, y3, y4, z4, y5, z5;
  cellg u1(a, b, y1, z1);
  cellg u2(a, b, y2, z2);
  cellp u3(a, y3);
  cellg u4(a, b, y4, z4);
  cellg u5(a, b, y5, z5);
  initial begin
    $sdf_annotate("data/sdf/32.4.1-device-primitives.sdf", top);
    #20 a = 1; b = 1;
    #20 a = 0;
    #20 $finish;
  end
  always @(y1, z1) if ($time >= 20) $display("%0t: u1 y=%b z=%b", $time, y1, z1);
  always @(y2, z2) if ($time >= 20) $display("%0t: u2 y=%b z=%b", $time, y2, z2);
  always @(y3) if ($time >= 20) $display("%0t: u3 y=%b", $time, y3);
  always @(y4, z4) if ($time >= 20) $display("%0t: u4 y=%b z=%b", $time, y4, z4);
  always @(y5, z5) if ($time >= 20) $display("%0t: u5 y=%b z=%b", $time, y5, z5);
endmodule
//> 20: u1 y=0 z=0
//> 21: u4 y=1 z=1
//> 22: u5 y=1 z=1
//> 23: u1 y=1 z=0
//> 24: u2 y=1 z=0
//> 27: u3 y=1
//> 27: u5 y=1 z=0
//> 29: u4 y=1 z=0
//> 40: u1 y=1 z=1
//> 41: u4 y=0 z=0
//> 43: u5 y=1 z=1
//> 44: u2 y=0 z=1
//> 45: u1 y=0 z=1
//> 46: u5 y=0 z=1
//> 47: u3 y=0
//> 49: u4 y=0 z=1
