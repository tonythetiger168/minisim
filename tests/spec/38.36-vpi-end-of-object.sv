// IEEE 1800-2023 38.36.1: cbEndOfObject on a transient object runs when it
// ends, as cbEndOfFrame or cbEndOfThread on it would, with no time passed.
// It needs a thread, frame or class object handle; cbReclaimObj needs a
// class definition or class object (msim never reclaims class objects, so
// that one does not run).
//! icarus-skip: Icarus 13 has no thread or frame objects
//! args: data/vpi/endobj.c
module top;
  task automatic work;
    $watch;
    #5 $display("%0t: task returns", $time);
  endtask
  initial begin
    fork
      begin
        work();
        #5 $display("%0t: thread ends", $time);
      end
    join
    #1 $display("%0t: done", $time);
  end
endmodule
//> no object: vpi_register_cb: cbEndOfObject needs a thread, frame or class object handle
//> reclaim of a module: vpi_register_cb: cbReclaimObj needs a class definition or class object handle
//> 5: task returns
//> 5: end of frame (time NULL)
//> 10: thread ends
//> 10: end of thread (time NULL)
//> 11: done
