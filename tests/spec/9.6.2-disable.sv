// IEEE 1800-2023 9.6.2: disable of the enclosing named block jumps past it;
// disable of a named fork ends the branches still running.
module top;
  integer n = 0;
  initial begin
    begin : blk
      repeat (5) begin
        n = n + 1;
        if (n == 2) disable blk;
      end
      $display("not reached");
    end
    $display("%0d n=%0d", $time, n);
    fork : watchdog
      begin #100 $display("timeout"); end
      begin #3 $display("%0d work done", $time); end
    join_any
    disable watchdog;
    $display("%0d after disable", $time);
    #10 $display("%0d end", $time);
  end
endmodule

//> 0 n=2
//> 3 work done
//> 3 after disable
//> 13 end
