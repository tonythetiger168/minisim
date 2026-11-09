// IEEE 1800-2023 20.4: $timeformat sets the units, precision, suffix and
// width that %t uses (20.4.3).
`timescale 1ns/1ps
module top;
  initial begin
    #5.25;
    $display("[%t]", $realtime);
    $timeformat(-9, 2, " ns", 10);
    $display("[%t]", $realtime);
    $timeformat(-12, 0, " ps", 0);
    $display("[%t]", $realtime);
  end
endmodule

//> [                5250]
//> [   5.25 ns]
//> [5250 ps]
