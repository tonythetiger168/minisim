// IEEE 1800-2023 9.6.3: disable fork ends every process this one forked.
module top;
  initial begin
    fork
      #50 $display("never");
      #1 $display("%0d c1", $time);
    join_none
    #2 disable fork;
    $display("%0d disabled", $time);
  end
endmodule

//> 1 c1
//> 2 disabled
