// IEEE 1800-2023 12.7.1: for loops declaring several variables, with
// several steps (12.7.1).
//! icarus-skip: Icarus 13 does not parse two loop variables
module top;
  initial for (int i = 0, j = 5; i < 2; i++, j--) $display("%0d %0d", i, j);
endmodule

//> 0 5
//> 1 4
