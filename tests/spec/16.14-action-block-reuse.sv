// IEEE 1800-2023 16.14.3 (action blocks): a pass action runs on every
// success, in the reactive region. msim frees each run when it finishes;
// one that waits keeps running to its end. 100 clocks: 100 passes, and the
// waiting half of each action finishes one clock later.
//! icarus-skip: Icarus 13 does not support concurrent assertions
module top;
  bit clk;
  int n = 0, m = 0;
  always #5 clk = ~clk;
  a1: assert property (@(posedge clk) 1) n++;
  a2: assert property (@(posedge clk) 1) begin @(negedge clk); m++; end
  initial begin
    #1003;
    $display("%0d %0d", n, m);
    $finish;
  end
endmodule

//> 100 100
