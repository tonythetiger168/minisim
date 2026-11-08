// IEEE 1800-2023 9.6.1: wait fork waits for the processes this one forked.
module top;
  initial begin
    fork
      #2 $display("%0d c1", $time);
      #4 $display("%0d c2", $time);
    join_none
    $display("%0d forked", $time);
    wait fork;
    $display("%0d after wait fork", $time);
  end
endmodule

//> 0 forked
//> 2 c1
//> 4 c2
//> 4 after wait fork
