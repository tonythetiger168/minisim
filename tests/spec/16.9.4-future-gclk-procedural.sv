// IEEE 1800-2023 16.9.4 with 16.14.6: a procedural concurrent assertion
// that uses a global clocking future sampled value function. Each
// instance is queued with the automatic values of the procedure then
// (16.14.6.1) and is decided at the next global clock tick, where its
// action block runs.
//! icarus-skip: Icarus has no global clocking future functions
module t;
  bit clk, a, en;
  int d;
  global clocking gc @(posedge clk); endclocking
  always #5 clk = !clk;
  always @(posedge clk) begin
    automatic int k = d;   // d now: the next tick d must be one more
    if (en) a_p: assert property (@(posedge clk) $future_gclk(d) == k + 1)
                 $display("%0t pass k=%0d", $time, k); else $display("%0t fail k=%0d d=%0d", $time, k, d);
  end
  initial begin
    @(negedge clk) en = 1;
    repeat (3) @(negedge clk) d++;
    @(negedge clk) d += 3;
    @(negedge clk) en = 0;
    #30 $finish;
  end
endmodule
//> 25 pass k=0
//> 35 pass k=1
//> 45 pass k=2
//> 55 fail k=3 d=6
//> 65 fail k=6 d=6
