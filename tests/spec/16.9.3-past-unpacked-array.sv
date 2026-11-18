//! icarus-skip: Icarus has no concurrent assertions
// IEEE 1800-2023 16.9.3: $past and $sampled of an unpacked array. The
// sampled value of an array is the array of its elements' sampled values,
// so the past value can be compared with another array (7.6, 11.4.5).
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic [3:0][7:0] a [2];
  logic [3:0][7:0] b [2];
  int fails = 0, passes = 0;
  // b may only take the value a had one clock earlier.
  assert property (@(posedge clk) ##1 (b == $past(b) || b == $past(a)))
    passes++; else begin fails++; $display("%0t: b=%p", $time, b); end
  always @(posedge clk) if ($sampled(a) != $past(a, 2)) $display("%0t: a moved", $time);
  initial begin
    a = '{0, 0}; b = '{0, 0};
    @(negedge clk) a = '{32'h01, 32'h02};
    @(negedge clk) b = '{32'h01, 32'h02};  // a's value at the last edge: passes
    @(negedge clk) b = '{32'h05, 32'h06};  // a never held this: fails
    repeat (2) @(negedge clk);
    $display("passes=%0d fails=%0d", passes, fails);
    $finish;
  end
endmodule
//> 25: a moved
//> 35: b='{5, 6}
//> passes=3 fails=1
