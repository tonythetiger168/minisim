// IEEE 1800-2023 38.36.1, annex M: thread and frame callbacks and objects.
// vpi_handle(vpiThread / vpiFrame, NULL) in a calltf gives the calling
// thread and its innermost task or function call. cbStartOfThread on a
// thread fires as it forks a child (obj: the new thread); cbEndOfThread and
// cbEnterThread (obj NULL: every thread) fire as a thread ends and as it
// runs again after a wait; cbStartOfFrame / cbEndOfFrame fire as a task or
// function call starts and returns (obj: the frame, with vpiScope the task
// or function, vpiParent its caller's frame and vpiThread its thread). An
// ended thread's handle has vpiValid 0.
//! icarus-skip: Icarus 13 has no thread or frame callbacks
//! args: data/vpi/threads.c
module top;
  function automatic int sq(int x);
    return x * x;
  endfunction
  task automatic work(input int n);
    #1 $where(n);
    r = sq(n);
  endtask
  int r;
  initial begin
    $watch;
    fork
      work(2);
      begin #3 r = 0; end
    join
    #1 $check;
  end
endmodule
//> watch: main thread, frame 0
//> start: thread T1 forked by main at 0
//> start: thread T2 forked by main at 0
//> frame start: work (automatic 1) in T1, caller - at 0
//> enter: T1 at 1
//> where 2: frame work, same thread 1, parent main, main has 2 children
//> frame start: sq (automatic 1) in T1, caller work at 1
//> frame end: sq in T1 at 1
//> frame end: work in T1 at 1
//> end: T1 at 1
//> enter: T2 at 3
//> end: T2 at 3
//> enter: main at 3
//> enter: main at 4
//> check: T1 valid 0, main valid 1
//> end: main at 4
