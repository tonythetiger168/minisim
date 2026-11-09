// IEEE 1800-2023 20.12: $rose, $fell, $stable, $past use values sampled at
// the clock (20.12).
//! icarus-skip: Icarus 13 does not support sampled value functions
module top;
  logic clk = 0, a = 0;
  always #5 clk = ~clk;
  initial begin #12 a = 1; #20 a = 0; end
  always @(posedge clk)
    $display("%0t r%0d f%0d s%0d p%0d", $time, $rose(a), $fell(a), $stable(a), $past(a));
  initial #50 $finish;
endmodule

//> 5 r0 f0 s1 p0
//> 15 r1 f0 s0 p0
//> 25 r0 f0 s1 p1
//> 35 r0 f1 s0 p1
//> 45 r0 f0 s1 p0
