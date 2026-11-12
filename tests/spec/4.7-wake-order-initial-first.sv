// IEEE 1800-2023 4.7, 9.4.2: when one event resumes several processes, the
// order is not defined by the standard. msim resumes processes that wait in
// the middle of their body (an initial past a wait, a task) before always
// procedures woken at their leading event control, the order other
// simulators use, so a value an initial sets on a clock edge is seen by the
// always blocks on that same edge.
//! icarus-skip: Icarus orders these wakeups another way (the standard leaves it open)
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic go = 0;
  int seen_a = 0, seen_b = 0;
  always @(negedge clk) if (go) seen_a++;
  initial begin
    repeat (2) @(negedge clk);
    go = 1;
    repeat (3) @(negedge clk);
    go = 0;
    #1 $display("a %0d b %0d", seen_a, seen_b);
    $finish;
  end
  always @(negedge clk) if (go) seen_b++;
endmodule
//> a 3 b 3
