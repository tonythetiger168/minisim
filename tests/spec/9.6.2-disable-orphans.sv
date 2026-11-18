// IEEE 1800-2023 9.6.2: disabling a named block ends every activity enabled
// within it, including processes a branch forked with join_none and then
// outlived. The process that runs the disable is one of them.
module top;
  initial begin
    fork : outer
      begin
        fork
          begin #2 disable outer; $display("not stopped: disabler"); end
          begin #3 $display("not stopped: sibling"); end
        join_none
      end
      begin #4 $display("not stopped: branch"); end
    join_none
    #1 $display("t=%0t forked", $time);
    #9 $display("t=%0t end", $time);
  end
endmodule
//> t=1 forked
//> t=10 end
