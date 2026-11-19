// IEEE 1800-2023 14.16.2: when several synchronous drives of the same
// clocking output mature in the same Re-NBA region, only the last value is
// driven onto the signal, so the signal does not change to the earlier
// values first (no event for them). Also for drives made between clocking
// events, which all mature at the next one; with an output skew.
//! icarus-skip: Icarus does not support clocking blocks
module top;
  bit clk = 0;
  always #5 clk = ~clk;
  logic [3:0] n = 0;
  int changes = 0;
  clocking cb @(posedge clk);
    default output #1;
    output n;
  endclocking
  always @(n) begin
    changes++;
    $display("%0t n=%h", $time, n);
  end
  initial begin
    @(cb);
    cb.n <= 4'h5;
    cb.n <= 4'h3;
    #2 cb.n <= 4'h1;
    #2 cb.n <= 4'h3;   // both mature at the next clocking event: n stays 3, no event
    @(cb);
    @(cb);
    $display("changes=%0d", changes);
    $finish;
  end
endmodule
//> 6 n=3
//> changes=1
