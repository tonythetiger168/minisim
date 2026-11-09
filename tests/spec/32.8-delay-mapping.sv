`timescale 1ns/1ns
// IEEE 1800-2023 32.8, Table 32-4: 1, 2, 3, 6 or 12 SDF delay values become
// the 12 transition delays of a specify path. A transition whose value
// comes from an empty "()" keeps its delay from before (u5's rise stays 4).
//! icarus-skip: Icarus 13 does not back-annotate specify paths
module tribuf(input a, en, output y);
  assign y = en ? a : 1'bz;
  specify
    (a, en *> y) = 4;
  endspecify
endmodule
module top;
  reg a1, a2, a3, a4, a5, e1, e2, e3, e4, e5;
  wire y1, y2, y3, y4, y5;
  tribuf u1(a1, e1, y1);   // (3) (5)
  tribuf u2(a2, e2, y2);   // (3) (5) (7)
  tribuf u3(a3, e3, y3);   // (3) (5) (7) (9) (11) (13)
  tribuf u4(a4, e4, y4);   // (1) ... (12)
  tribuf u5(a5, e5, y5);   // () (9)
  // Each instance in turn takes every transition: 0->1, 1->z, z->1, 1->0,
  // 0->z, z->0, 0->x, x->1, 1->x, x->0, 0->x, x->z, z->x, 20 apart.
  task automatic run(ref reg a, en);
    #20 a = 1;
    #20 en = 0;
    #20 en = 1;
    #20 a = 0;
    #20 en = 0;
    #20 en = 1;
    #20 a = 1'bx;
    #20 a = 1;
    #20 a = 1'bx;
    #20 a = 0;
    #20 a = 1'bx;
    #20 en = 0;
    #20 en = 1;
  endtask
  initial begin
    $sdf_annotate("data/sdf/32.8-delay-mapping.sdf", top);
    {a1, a2, a3, a4, a5} = 0;
    {e1, e2, e3, e4, e5} = '1;
    #100 run(a1, e1);
    #140 run(a2, e2);
    #140 run(a3, e3);
    #140 run(a4, e4);
    #140 run(a5, e5);
    #100 $finish;
  end
  always @(y1) if ($time >= 100) $display("%0t u1=y%b", $time, y1);
  always @(y2) if ($time >= 100) $display("%0t u2=y%b", $time, y2);
  always @(y3) if ($time >= 100) $display("%0t u3=y%b", $time, y3);
  always @(y4) if ($time >= 100) $display("%0t u4=y%b", $time, y4);
  always @(y5) if ($time >= 100) $display("%0t u5=y%b", $time, y5);
endmodule

//> 123 u1=y1
//> 145 u1=yz
//> 163 u1=y1
//> 185 u1=y0
//> 203 u1=yz
//> 225 u1=y0
//> 243 u1=yx
//> 263 u1=y1
//> 285 u1=yx
//> 305 u1=y0
//> 323 u1=yx
//> 345 u1=yz
//> 363 u1=yx
//> 523 u2=y1
//> 547 u2=yz
//> 563 u2=y1
//> 585 u2=y0
//> 607 u2=yz
//> 625 u2=y0
//> 643 u2=yx
//> 663 u2=y1
//> 685 u2=yx
//> 705 u2=y0
//> 723 u2=yx
//> 747 u2=yz
//> 763 u2=yx
//> 923 u3=y1
//> 951 u3=yz
//> 969 u3=y1
//> 985 u3=y0
//> 1007 u3=yz
//> 1033 u3=y0
//> 1043 u3=yx
//> 1069 u3=y1
//> 1085 u3=yx
//> 1113 u3=y0
//> 1123 u3=yx
//> 1151 u3=yz
//> 1169 u3=yx
//> 1321 u4=y1
//> 1345 u4=yz
//> 1364 u4=y1
//> 1382 u4=y0
//> 1403 u4=yz
//> 1426 u4=y0
//> 1447 u4=yx
//> 1468 u4=y1
//> 1489 u4=yx
//> 1510 u4=y0
//> 1527 u4=yx
//> 1551 u4=yz
//> 1572 u4=yx
//> 1724 u5=y1
//> 1749 u5=yz
//> 1764 u5=y1
//> 1789 u5=y0
//> 1804 u5=yz
//> 1829 u5=y0
//> 1844 u5=yx
//> 1864 u5=y1
//> 1889 u5=yx
//> 1909 u5=y0
//> 1924 u5=yx
//> 1944 u5=yz
//> 1964 u5=yx
