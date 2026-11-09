// IEEE 1800-2023 24.7: $exit ends every initial procedure of the calling
// program and the threads they forked. Another program runs on, and the
// run ends when it ends.
module top;
  p1 a();
  p2 b();
endmodule

program p1;
  initial begin
    fork
      begin #2 $display("%0t p1 child exits", $time); $exit; end
    join_none
    #10 $display("%0t p1 not reached", $time);
  end
  initial #4 $display("%0t p1 second not reached", $time);
endprogram

program p2;
  initial begin
    #5 $display("%0t p2 runs on", $time);
  end
endprogram

//> 2 p1 child exits
//> 5 p2 runs on
