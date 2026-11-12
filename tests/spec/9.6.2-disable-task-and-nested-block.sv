// IEEE 1800-2023 9.6.2: disable of a task ends every running call of it,
// and the tasks it called; the caller goes on after the call. A named
// block nested inside another process can be disabled from outside, which
// ends that block only, and everything a block forked ends with it.
module top;
  int hits = 0;
  logic clk = 0, rst = 0;
  always #5 clk = ~clk;

  task inner_wait(input int n);
    repeat (n) @(posedge clk);
    hits++;
  endtask
  task automatic outer_wait(input int n, input string who);
    inner_wait(n);
    $display("%0t %s finished", $time, who);
  endtask
  task check_early(input int v);
    if (v < 0) disable check_early;   // inside itself: like return
    $display("%0t check %0d", $time, v);
  endtask

  // A watchdog: the body restarts whenever rst pulses.
  always begin : watch
    begin : count_down
      repeat (3) @(posedge clk);
      $display("%0t watchdog fired", $time);
    end
    @(posedge clk);
  end
  always @(posedge rst) disable watch.count_down;

  initial begin
    fork
      begin outer_wait(4, "a"); $display("%0t after a", $time); end
      begin outer_wait(6, "b"); $display("%0t after b", $time); end
    join_none
    #12 disable outer_wait;
    #1 $display("%0t hits=%0d", $time, hits);
    check_early(-1);
    check_early(2);
    begin : spawner
      fork
        #3 $display("%0t never printed", $time);
      join_none
      #1 disable spawner;
    end
    #5 $display("%0t after spawner", $time);
    #4 rst = 1; #1 rst = 0;
    #40 $finish;
  end
endmodule

//> 12 after a
//> 12 after b
//> 13 hits=0
//> 13 check 2
//> 19 after spawner
//> 55 watchdog fired
