// IEEE 1800-2023 16.5.1: a concurrent assertion reads sampled values, also
// through a bit select or an indexed part select with a variable index: at
// the edge where r changes, r[k] is still the old r.
//! icarus-skip: Icarus 13 does not support concurrent assertions with action blocks
module top;
  logic clk = 0;
  logic [3:0] r = 4'b0000;
  logic [1:0] k = 2'd3;
  always #5 clk = ~clk;
  always_ff @(posedge clk) r <= 4'b1001;
  a_bit: assert property (@(posedge clk) r[k]) else $display("%0t bit fails", $time);
  a_part: assert property (@(posedge clk) r[k -: 2] == 2'b10) else $display("%0t part fails", $time);
  a_not: assert property (@(posedge clk) !r[k]) else $display("%0t not fails", $time);
  initial #22 $finish;
endmodule

//> 5 bit fails
//> 5 part fails
//> 15 not fails
