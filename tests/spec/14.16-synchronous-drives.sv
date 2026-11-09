// IEEE 1800-2023 14.16, 14.16.2: synchronous drives with cycle delays. A
// drive at a clocking event matures at it; one between events matures at the
// next; drives that mature together leave the last value (the 14.16.2 example).
//! icarus-skip: Icarus 13 does not support clocking blocks
module top;
  logic clk = 0;
  always #5 clk = ~clk;   // clocking events at 5, 15, 25, ...
  bit [7:0] v;
  default clocking cb @(posedge clk);
    output v;
  endclocking
  initial begin
    ##1;                  // cycle 1 (5)
    cb.v <= 1;            // matures in cycle 1
    cb.v <= ##2 2;        // cycle 3
    #1 cb.v <= ##2 3;     // at 6: cycle 3
    ##1 cb.v <= ##1 4;    // at 15: cycle 3; the last drive wins
    #13 cb.v <= 5;        // at 28, between events: the next one (35)
  end
  always @(v) $display("%0t v=%0d", $time, v);
  initial #50 $finish;
endmodule

//> 5 v=1
//> 25 v=4
//> 35 v=5
