//! icarus-skip: Icarus has no sampled value functions on arrays
// IEEE 1800-2023 16.9.3: $stable and $changed of a fixed-size unpacked
// array compare every element's sampled value with its value at the
// previous tick of the clock.
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic [7:0] a [2][2];
  global clocking @(posedge clk); endclocking
  always @(posedge clk)
    if ($time > 5) $display("%0t: stable=%b changed=%b gclk=%b", $time, $stable(a), $changed(a), $changed_gclk(a));
  assert property (@(posedge clk) $time > 5 |-> $stable(a[0])) else $display("%0t: fail", $time);
  initial begin
    a = '{'{1, 2}, '{3, 4}};
    @(negedge clk);
    @(negedge clk) a[1][0] = 8'h05;
    @(negedge clk) a[0][1] = 8'h06;
    @(negedge clk);
    $finish;
  end
endmodule
//> 15: stable=1 changed=0 gclk=0
//> 25: stable=0 changed=1 gclk=1
//> 35: stable=0 changed=1 gclk=1
//> 35: fail
