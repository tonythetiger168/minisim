// IEEE 1800-2023 16.13.3, 16.13.4: multiclock properties: and of properties on
// two clocks, and nexttime counting ticks of the clock in effect.
//! icarus-skip: Icarus 13 does not support concurrent assertions
module top;
  logic c0 = 0, c1 = 0;
  always #5 c0 = ~c0;            // posedges at 5, 15, 25, ...
  initial begin #2; forever #7 c1 = ~c1; end   // posedges at 9, 23, 37, 51 ...
  logic a = 0, b = 0, c = 0;
  initial $assertvacuousoff;
  // and of properties on two clocks; each side waits for its own clock (16.13.3)
  p3: assert property (@(posedge c0) a |-> (@(posedge c1) b) and (@(posedge c0) ##1 c))
    $display("%0t p3 pass", $time); else $display("%0t p3 FAIL", $time);
  // a clock that only appears inside: nexttime counts ticks of its own clock
  p4: assert property (@(posedge c0) a |-> @(posedge c1) nexttime[2] c)
    $display("%0t p4 pass", $time); else $display("%0t p4 FAIL", $time);
  initial begin
    #12 a = 1; b = 1; // a at 15; b at c1 23
    #5 a = 0;         // 17
    #5 c = 1;         // 22: c at c0 25 -> p3 pass at 25 (b at 23)
    #30 c = 0;        // 52: c at c1 ticks 37 (nexttime 0: 23, 1: 37, 2: 51) -> p4 pass at 51
    #20 $finish;
  end
endmodule

//> 25 p3 pass
//> 51 p4 pass
