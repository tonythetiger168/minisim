// IEEE 1800-2023 14.11: ## waits for cycles of the default clocking (14.11,
// 14.12).
//! xfail: ## is an error
//! icarus-skip: Icarus 13 does not support clocking blocks
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  default clocking cb @(posedge clk); endclocking
  initial begin
    ##2;
    $display("%0t", $time);
    $finish;
  end
endmodule

//> 15
