// IEEE 1800-2023 6.21, 9.3.2: an automatic variable declared in a block is
// made new each time the block is entered, and a process forked inside the
// block keeps using that one. Forks in a loop body therefore see each pass's
// value, while code of the same pass shares the variable with the fork.
//! icarus-skip: Icarus aborts on fork/join_none of automatic tasks (vvp of_JOIN_DETACH assertion)
module t;
  int q[3];
  // foreach body in an automatic task: one p per pass
  task automatic each();
    foreach (q[i]) begin
      automatic int p = i;
      fork #1 $display("each p=%0d", p); join_none
    end
  endtask
  // for body, the forked process nested one fork deeper
  task automatic nested();
    for (int i = 0; i < 3; i++) begin
      int p = i * 10;
      fork
        fork #2 $display("nested p=%0d", p); join_none
      join_none
    end
  endtask
  // the same pass changes p after the fork: the fork sees the change
  task automatic shared();
    begin
      int p = 1;
      fork #3 $display("shared p=%0d", p); join_none
      p = 7;
    end
  endtask
  // a forked process writes the block's variable, the block reads it after the join
  task automatic back();
    begin
      int r = 0;
      fork begin #4 r = 5; end join
      $display("back r=%0d", r);
    end
  endtask
  // two processes of one fork share the pass's variable
  task automatic pair();
    for (int i = 0; i < 2; i++) begin
      int p = i;
      fork
        begin #5 p = p + 100; end
        begin #6 $display("pair p=%0d", p); end
      join_none
    end
  endtask
  initial begin
    each(); nested(); shared(); back(); pair();
    #10;
  end
endmodule
//> each p=0
//> each p=1
//> each p=2
//> nested p=0
//> nested p=10
//> nested p=20
//> shared p=7
//> back r=5
//> pair p=100
//> pair p=101
