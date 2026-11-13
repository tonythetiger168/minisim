module Adder #(parameter int W = 8) (input logic [W-1:0] A, B, output logic [W:0] Sum);
  assign Sum = A + B;
endmodule
