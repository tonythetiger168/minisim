// IEEE 1800-2023 14.3, 14.4: an input skew may be an edge of the clock with
// a delay: `input negedge #2` samples the value the signal had 2 time units
// before the last falling edge of the clock ahead of the clocking event.
//! icarus-skip: Icarus has no clocking blocks
module t;
  bit clk;
  int d;
  always #5 clk = !clk;     // rising at 5, 15, 25 ...; falling at 10, 20 ...
  always #1 d = d + 1;      // d is t at the end of time t
  clocking cb @(posedge clk);
    input negedge #2 e = d;
    input #2 f = d;
  endclocking
  initial begin
    repeat (3) @(cb);
    $display("%0t e=%0d f=%0d", $time, cb.e, cb.f);
    @(cb);
    $display("%0t e=%0d f=%0d", $time, cb.e, cb.f);
    $finish;
  end
endmodule
//> 25 e=18 f=23
//> 35 e=28 f=33
