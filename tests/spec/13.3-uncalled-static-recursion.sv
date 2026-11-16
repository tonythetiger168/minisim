// IEEE 1800-2023 13.3: tasks and functions are static unless declared
// automatic, and their storage is shared by every call. A cycle of static
// tasks that no process calls is never entered, so it does not stop the
// design from being elaborated and run.
module top;
  logic clk = 0;
  always #5 clk = ~clk;

  task static wait_edges(input int n);
    repeat (n) @(posedge clk);
  endtask

  task static ping(input int k);
    if (k >= 0) wait_edges(k);
    else pong(k);
  endtask

  task static pong(input int k);
    int scratch;
    scratch = k;
    ping(scratch + 1);
  endtask

  initial begin
    wait_edges(3);
    $display("t=%0t", $time);
    $finish;
  end
endmodule
//> t=25
