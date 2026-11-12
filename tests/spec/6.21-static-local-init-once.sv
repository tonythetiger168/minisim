// IEEE 1800-2023 6.21: a static variable declared in a block of an always
// procedure, or in a loop body, is initialized once, before simulation; it
// is not set again each time the block is entered.
//! icarus-skip: Icarus does not support an automatic variable in a static block
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  always @(negedge clk) begin : counter
    static int hits = 0, twice = 10;
    hits++;
    twice += 2;
    if (hits == 4) $display("always hits %0d twice %0d", hits, twice);
  end
  initial begin
    for (int i = 0; i < 3; i++) begin : body
      static int total = 100;
      automatic int fresh = 7;
      total += i;
      fresh += i;
      $display("loop %0d total %0d fresh %0d", i, total, fresh);
    end
    #50 $finish;
  end
endmodule
//> loop 0 total 100 fresh 7
//> loop 1 total 101 fresh 8
//> loop 2 total 103 fresh 9
//> always hits 4 twice 18
