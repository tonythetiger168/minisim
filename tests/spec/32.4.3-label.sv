`timescale 1ns/100ps
// IEEE 1800-2023 32.4.3: an SDF LABEL gives a specparam a new value, and
// every expression that uses the specparam is computed with it. The two
// examples of 32.4.3: a clock whose procedural delays are specparams, and
// a path delay 1.4 * cap + 0.7. INCREMENT adds to the value so far.
//! icarus-skip: Icarus 13 does not annotate specparams
module clock(clk);
  output clk;
  reg clk;
  specparam dhigh = 0, dlow = 0;
  initial clk = 0;
  always
    begin
      #dhigh clk = 1;   // Clock remains low for time dlow before transitioning to 1
      #dlow clk = 0;    // Clock remains high for time dhigh before transitioning to 0
    end
endmodule
module capbuf(input A, output Z);
  specparam cap = 0;
  assign Z = A;
  specify
    (A => Z) = 1.4 * cap + 0.7;
  endspecify
endmodule
module top;
  wire clk, z;
  clock c(clk);
  capbuf u(clk, z);
  initial begin
    $sdf_annotate("data/sdf/32.4.3-label.sdf");
    #250 $finish;
  end
  always @(clk) $display("%.1f clk=%b", $realtime, clk);
  always @(z) if ($time > 0) $display("%.1f z=%b", $realtime, z);
endmodule

// dhigh 60, dlow 40; cap 2 + 1 = 3, so the path delay is 4.9.
//> 0.0 clk=0
//> 4.9 z=0
//> 60.0 clk=1
//> 64.9 z=1
//> 100.0 clk=0
//> 104.9 z=0
//> 160.0 clk=1
//> 164.9 z=1
//> 200.0 clk=0
//> 204.9 z=0
