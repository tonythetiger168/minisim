// IEEE 1800-2023 16.13.1, 16.13.2: multiclock sequences and implications:
// ##1 to another clock waits for its first tick strictly after, |-> for the
// first tick at or after.
//! icarus-skip: Icarus 13 does not support concurrent assertions
module top;
  logic c0 = 0, c1 = 0;
  always #5 c0 = ~c0;            // posedges at 5, 15, 25, ...
  initial begin #2; forever #7 c1 = ~c1; end   // posedges at 9, 23, 37, 51, 65 ...
  logic a = 0, b = 0, d = 0;
  initial $assertvacuousoff;
  // ##1 between clocks: b at the first c1 tick strictly after a (16.13.1)
  s1: cover property (@(posedge c0) a ##1 @(posedge c1) b) $display("%0t s1 match", $time);
  // |=> across clocks: b at the first c1 tick strictly after the antecedent
  p1: assert property (@(posedge c0) a |=> @(posedge c1) b)
    $display("%0t p1 pass", $time); else $display("%0t p1 FAIL", $time);
  // |-> across clocks: at or after (the same tick when both clocks tick)
  p2: assert property (@(posedge c0) d |-> @(posedge c1) b ##1 !b)
    $display("%0t p2 pass", $time); else $display("%0t p2 FAIL", $time);
  initial begin
    #12 a = 1;        // a sampled at 15
    #5 a = 0;         // 17
    #4 b = 1;         // 21: b sampled at 23 -> s1, p1 pass at 23
    #4 b = 0;         // 25
    #10 a = 1;        // 35: a at 45 (c0); next c1 tick 51: b = 0 -> p1 FAIL at 51
    #10 a = 0;        // 45... a sampled at 45 = 1
    #20 d = 1; b = 1; // 65: d at 75 (c0); c1 tick 79: b = 1, then 93: !b
    #10 d = 0;        // 75
    #10 b = 0;        // 85
    #20 $finish;
  end
endmodule

//> 23 s1 match
//> 23 p1 pass
//> 51 p1 FAIL
//> 93 p2 pass
