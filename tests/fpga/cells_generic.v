// Behavioural models of the generic (Yosys) cells mfpga writes, for the simulation tests.
// They follow the vendor's simulation library for the pins mfpga uses.
module \$lut #(parameter WIDTH = 1, parameter [63:0] LUT = 0) (input [WIDTH-1:0] A, output Y);
  // a chain of ?: selects, so an x input that does not matter gives no x
  wire [63:0] s [0:WIDTH];
  assign s[WIDTH] = LUT;
  genvar k;
  generate
    for (k = 0; k < WIDTH; k = k + 1) begin : g
      assign s[k] = A[k] ? (s[k + 1] >> (64'd1 << k)) & ((64'd1 << (64'd1 << k)) - 64'd1) : s[k + 1] & ((64'd1 << (64'd1 << k)) - 64'd1);
    end
  endgenerate
  assign Y = s[0][0];
endmodule
module \$_DFF_P_ (input C, D, output reg Q); always @(posedge C) Q <= D; endmodule
module \$_DFF_N_ (input C, D, output reg Q); always @(negedge C) Q <= D; endmodule
module \$_DFF_PP0_ (input C, D, R, output reg Q); always @(posedge C or posedge R) if (R) Q <= 0; else Q <= D; endmodule
module \$_DFF_PP1_ (input C, D, R, output reg Q); always @(posedge C or posedge R) if (R) Q <= 1; else Q <= D; endmodule
module \$_DFF_NP0_ (input C, D, R, output reg Q); always @(negedge C or posedge R) if (R) Q <= 0; else Q <= D; endmodule
module \$_DFF_NP1_ (input C, D, R, output reg Q); always @(negedge C or posedge R) if (R) Q <= 1; else Q <= D; endmodule
