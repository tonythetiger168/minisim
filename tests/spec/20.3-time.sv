// IEEE 1800-2023 20.3 Simulation time system functions.
module t;
  initial begin
    $display("%0t", $time);
    #7 $display("%0d %0d", $time, $stime);
    #3 $display("[%t]", $time);
  end
endmodule
//> 0
//> 7 7
//> [                  10]
