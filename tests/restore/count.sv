// msim -restore (tests/msim_restore.cmake): a counter, a $save at time 52,
// and a plusarg read before the checkpoint (+early) and one after (+late).
module top;
  int n = 0, e = 0;
  logic clk = 0;
  always #5 clk = ~clk;
  always @(posedge clk) n <= n + 1 + e;
  initial begin
    e = $test$plusargs("early");
    #52 $display("before %0d", n);
    $save("count.chk");
    $display("after save %0d late=%0d", n, $test$plusargs("late"));
    #50 $display("end %0d", n);
    $finish;
  end
endmodule
