// IEEE 1800-2023 19.3: A covergroup with a coverpoint sampled on a clock,
// and get_coverage (19.3, 19.5, 19.8).
//! icarus-skip: Icarus 13 does not support covergroups
module top;
  logic clk = 0;
  bit [1:0] v;
  always #5 clk = ~clk;
  covergroup cg @(posedge clk);
    coverpoint v;
  endgroup
  cg c = new;
  initial begin
    v = 0; @(negedge clk) v = 1;
    @(negedge clk) $display("%0.1f", c.get_coverage());
    $finish;
  end
endmodule

//> 50.0
