// IEEE 1800-2023 6.5, 9.2.2.4: a variable written by several continuous
// assignments, or by an always_ff and another process, is illegal. msim
// warns and runs such a design (as other simulators do); -strict-drivers
// makes these errors, found before the run.
//! icarus-skip: Icarus has no -strict-drivers
//! args: -strict-drivers
//! exit: 1
module top;
  logic clk = 0, q;
  logic v;
  assign v = 1'b1;
  assign v = 1'b0;
  always_ff @(posedge clk) q <= v;
  initial q = 0;
  initial $display("not reached");
endmodule
