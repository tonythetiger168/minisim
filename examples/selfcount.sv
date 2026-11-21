// msim -compile: a block shaped like combinational logic that computes a
// signal from that signal's old value (always @(q) n = n + 1: a counter of
// q's changes) is not a function of its inputs. The cycle domain's comb
// pass re-runs every comb function on each clock edge, so this one must
// stay a process: n counts the 3 changes of q, not 20 clock edges. A latch
// (keeps its old value on one path) and an always_comb with defaults stay
// comb functions.
module selfcount;
  logic clk = 0;
  logic [7:0] q, d, l, m;
  logic en;
  int n = 0;
  always #5 clk = ~clk;
  always_ff @(posedge clk) q <= d;
  always @(q) n = n + 1;
  always_latch if (en) l = q;
  always_comb begin
    if (q[0]) m = q + 1; else m = 0;
    m = m + 2;   // reads m after it is written on every path
  end
  initial begin
    d = 0; en = 1;
    repeat (10) @(negedge clk);
    d = 1;
    repeat (5) @(negedge clk);
    en = 0; d = 6;
    repeat (5) @(negedge clk);
    $display("n=%0d l=%0d m=%0d", n, l, m);
    $finish;
  end
endmodule
