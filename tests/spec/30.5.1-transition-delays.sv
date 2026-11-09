`timescale 1ns/1ns
// IEEE 1800-2023 30.5.1, 30.5.2: a path delay with 6 values gives each
// transition among 0, 1 and z its own delay (Table 30-2), and the x
// transitions are worked out by Table 30-3 (the usage example there:
// (5, 12, 17, 10, 6, 22)). With 12 values every transition is given.
//! icarus-skip: Icarus 13 ignores specify delays unless run with -gspecify
module six(input a, en, output y);
  assign y = en ? a : 1'bz;
  specify
    (a, en *> y) = (5, 12, 17, 10, 6, 22);
  endspecify
endmodule
module twelve(input a, en, output y);
  assign y = en ? a : 1'bz;
  specify
    (a, en *> y) = (10, 12, 14, 15, 29, 36, 14, 15, 15, 14, 20, 30);
  endspecify
endmodule
module top;
  reg a1 = 0, en1 = 1, a2 = 0, en2 = 1;
  wire y, w;
  six u1(a1, en1, y);
  twelve u2(a2, en2, w);
  always @(y) $display("%0t y=%b", $time, y);
  always @(w) $display("%0t w=%b", $time, w);
  task automatic run(ref reg a, en);
    #30 a = 1;            // 0->1
    #20 en = 0;           // 1->z
    #20 en = 1;           // z->1
    #20 a = 0;            // 1->0
    #20 en = 0;           // 0->z
    #20 en = 1;           // z->0
    #40 a = 1'bx;         // 0->x
    #20 a = 1;            // x->1
    #20 a = 1'bx;         // 1->x
    #20 a = 0;            // x->0
    #40 a = 1'bx;         // 0->x
    #20 en = 0;           // x->z
    #20 en = 1;           // z->x
  endtask
  initial begin
    run(a1, en1);
    #500 run(a2, en2);
    #100 $finish;
  end
endmodule

// 0 -> x at time 0 is x -> 0: Table 30-3 gives max(22, 12) = 22.
//> 0 y=x
//> 0 w=x
//> 14 w=0
//> 22 y=0
//> 35 y=1
//> 56 y=z
//> 80 y=1
//> 102 y=0
//> 127 y=z
//> 152 y=0
//> 175 y=x
//> 200 y=1
//> 216 y=x
//> 252 y=0
//> 275 y=x
//> 307 y=z
//> 320 y=x
// The 12-value path, started at 810. At 860 the 1->z delay is 29, so the
// z is still on its way when en returns at 880: the pulse is rejected (30.7).
//> 850 w=1
//> 912 w=0
//> 934 w=z
//> 976 w=0
//> 994 w=x
//> 1015 w=1
//> 1035 w=x
//> 1054 w=0
//> 1094 w=x
//> 1120 w=z
//> 1150 w=x
