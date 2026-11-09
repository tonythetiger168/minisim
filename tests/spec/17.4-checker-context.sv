// IEEE 1800-2023 17.2-17.4: a checker declared in a package, a checker
// instantiated inside another checker, a deferred assertion in a checker
// (checked whenever its expression changes), and context inference: the
// clock defaults to $inferred_clock, the module's default clocking.
//! icarus-skip: Icarus 13 has no checkers
package chk_pkg;
  checker onehot_chk(logic [3:0] v);
    a1: assert #0 ($onehot0(v)) else $display("%0t onehot0 failed: %b", $time, v);
  endchecker
endpackage

checker pair_chk(logic [3:0] a, logic [3:0] b, event clock = $inferred_clock);
  chk_pkg::onehot_chk ca(a);
  chk_pkg::onehot_chk cb(b);
  a2: assert property (@clock a != b) else $display("%0t a == b: %b", $time, a);
endchecker

module top;
  logic clk = 0;
  logic [3:0] x = 4'b0001, y = 4'b0010;
  default clocking @(posedge clk); endclocking
  always #5 clk = ~clk;
  pair_chk p(x, y);
  initial begin
    #12 x = 4'b0011;
    #2 x = 4'b0010;
    #10 $finish;
  end
endmodule
//> 12 onehot0 failed: 0011
//> 15 a == b: 0010
