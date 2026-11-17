// IEEE 1800-2023 9.6.3: disable fork ends every descendant of the calling
// process, including a process forked by a child that has itself already
// finished. Processes that are not descendants keep running.
//! icarus-skip: Icarus 13 leaves the descendants of a finished child running
module top;
  initial begin
    fork
      begin
        fork
          #3 $display("grandchild still ran");
        join_none
        #1;
      end
    join_none
    #2 disable fork;
  end
  initial #5 $display("other initial ran at %0t", $time);
endmodule
//> other initial ran at 5
