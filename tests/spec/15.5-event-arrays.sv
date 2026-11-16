// 15.5, 6.17, 8.3: fixed-size arrays of named events - in a module, as a
// class property (also two-dimensional and static), and in a task.
// Each element is its own event: a constant or run-time index names it for
// ->, ->>, @ and .triggered.
//! icarus-skip: Icarus does not support event arrays
class Pool;
  event done[2];
  event grid[0:1][3:2];
  static event shared[2];
  int seen;
  task automatic finish(int k); #1 ->done[k]; endtask
  task automatic watch(int a, int b); @(grid[a][b]); seen++; endtask
endclass
module top;
  event ev[3];
  event down[4:1];
  Pool p = new, q = new;
  int i = 1, hits;
  task automatic local_events();
    event t[2];
    fork #2 ->t[i]; join_none
    @(t[1]);
    $display("task array at %0t", $time);
  endtask
  initial begin
    fork p.finish(1); join_none
    @(p.done[1]);
    $display("property at %0t", $time);
    fork #1 ->ev[i]; join_none
    @(ev[1]);
    $display("module at %0t triggered %0d %0d", $time, ev[1].triggered, ev[0].triggered);
    fork #1 ->down[4]; join_none
    wait (down[4].triggered);
    $display("descending range at %0t", $time);
    fork q.watch(1, 3); join_none
    #1 ->q.grid[1][3];
    ->p.grid[1][3];
    #1 $display("two-dimensional %0d %0d", q.seen, p.seen);
    fork begin @(Pool::shared[i]); hits++; end join_none
    #1 ->Pool::shared[1];
    #0 #0 $display("static property %0d", hits);
    local_events();
    fork #1 ->>ev[2]; join_none
    @(ev[2]) $display("nonblocking at %0t", $time);
    fork begin #1 ->ev[0]; #1 ->ev[2]; end join_none
    wait_order(ev[0], ev[2]) $display("wait_order passed at %0t", $time);
    else $display("wait_order failed");
  end
endmodule
//> property at 1
//> module at 2 triggered 1 0
//> descending range at 3
//> two-dimensional 1 0
//> static property 1
//> task array at 8
//> nonblocking at 9
//> wait_order passed at 11
