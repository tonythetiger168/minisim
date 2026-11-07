// IEEE 1800-2023 9.2.3: a final block runs once when simulation ends.
module top;
  integer n = 0;
  initial repeat (3) #10 n = n + 1;
  final $display("final at %0d n=%0d", $time, n);
endmodule

//> final at 30 n=3
