// IEEE 1800-2023 23.3.3: An array of instances: each gets its slice of the
// port connections (23.3.3).
module inv(input logic a, output logic y);
  assign y = ~a;
endmodule
module top;
  logic [3:0] x = 4'b0110;
  wire [3:0] y;
  inv u[3:0] (.a(x), .y(y));
  initial #1 $display("%b", y);
endmodule

//> 1001
