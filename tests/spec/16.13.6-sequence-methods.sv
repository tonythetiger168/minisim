// IEEE 1800-2023 16.13.6, 9.4.2.4: a sequence's end point: .triggered in an
// assertion and in wait, @(sequence) in procedural code, and .matched read on
// another clock (true at its first tick after the match).
//! icarus-skip: Icarus 13 does not support sequence methods
module top;
  logic clk = 0, clk2 = 0;
  always #5 clk = ~clk;
  always #7 clk2 = ~clk2;
  logic a = 0, b = 0, c = 0;
  sequence s_ab;
    @(posedge clk) a ##1 b;
  endsequence
  // .triggered inside an assertion on the same clock (16.13.6)
  a_t: assert property (@(posedge clk) s_ab.triggered |-> c)
    $display("%0t t pass", $time); else $display("%0t t FAIL", $time);
  // @(s) and wait (s.triggered) in procedural code (9.4.2.4)
  initial forever begin
    @(s_ab) $display("%0t @(s_ab)", $time);
  end
  initial begin
    #1;
    wait (s_ab.triggered) $display("%0t wait triggered", $time);
  end
  // .matched from another clock
  a_m: cover property (@(posedge clk2) s_ab.matched) $display("%0t matched on clk2", $time);
  initial begin
    $assertvacuousoff;
    @(negedge clk) a = 1;          // 10
    @(negedge clk) a = 0; b = 1; c = 1;  // 20 -> match at 25
    @(negedge clk) b = 0; c = 0;   // 30
    @(negedge clk) a = 1;          // 40
    @(negedge clk) a = 0; b = 1;   // 50 -> match at 55, c = 0: fail
    @(negedge clk) b = 0;
    @(negedge clk);
    @(negedge clk) $finish;
  end
endmodule

//> 25 @(s_ab)
//> 25 wait triggered
//> 25 t pass
//> 35 matched on clk2
//> 55 @(s_ab)
//> 55 t FAIL
//> 63 matched on clk2
