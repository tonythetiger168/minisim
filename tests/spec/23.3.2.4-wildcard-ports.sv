// IEEE 1800-2023 23.3.2.4: .* connects ports to signals of the same name
// (23.3.2.4).
module m(input logic a, output logic b);
  assign b = ~a;
endmodule
module top;
  logic a = 1;
  wire b;
  m u(.*);
  initial #1 $display("%b", b);
endmodule

//> 0
