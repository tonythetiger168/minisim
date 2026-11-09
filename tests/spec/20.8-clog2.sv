// IEEE 1800-2023 20.8: $clog2 is the ceiling of log2 (20.8.1).
module top;
  initial $display("%0d %0d %0d %0d %0d", $clog2(0), $clog2(1), $clog2(2), $clog2(9), $clog2(1024));
endmodule

//> 0 0 1 4 10
