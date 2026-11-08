// IEEE 1800-2023 15.5.1, 15.5.2: -> e wakes every process waiting on @e.
module top;
  event e;
  initial begin
    @e $display("%0d a got e", $time);
    @e $display("%0d a got e again", $time);
  end
  initial @e #1 $display("%0d b got e (+1)", $time);
  initial begin
    #5 -> e;
    #5 -> e;
  end
endmodule

//> 5 a got e
//> 6 b got e (+1)
//> 10 a got e again
