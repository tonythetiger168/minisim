// IEEE 1800-2023 14.10, 9.4.2.3: the clocking block event @(cb) may be
// qualified with iff; the wait completes only on a clocking event at which
// the condition is true. Also for the global clocking event (14.14).
//! icarus-skip: Icarus does not support clocking blocks
module top;
  bit clk = 0, en = 0;
  always #5 clk = ~clk;
  clocking cb @(posedge clk); endclocking
  global clocking gc @(negedge clk); endclocking
  initial begin
    #12 en = 1;
    #20 en = 0;
  end
  initial begin
    repeat (2) begin
      @(cb iff en);
      $display("cb %0t", $time);
    end
    @($global_clock iff !en);
    $display("gc %0t", $time);
    $finish;
  end
endmodule
//> cb 15
//> cb 25
//> gc 40
