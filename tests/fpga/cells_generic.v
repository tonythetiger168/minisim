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
module \$_DFFE_PP_ (input C, D, E, output reg Q); always @(posedge C) if (E) Q <= D; endmodule
module \$_DFFE_PP0P_ (input C, D, R, E, output reg Q); always @(posedge C or posedge R) if (R) Q <= 0; else if (E) Q <= D; endmodule
module \$_SDFF_PP0_ (input C, D, R, output reg Q); always @(posedge C) if (R) Q <= 0; else Q <= D; endmodule
module \$_SDFFE_PP0P_ (input C, D, R, E, output reg Q); always @(posedge C) if (R) Q <= 0; else if (E) Q <= D; endmodule
module \$_DFFE_PP1P_ (input C, D, R, E, output reg Q); always @(posedge C or posedge R) if (R) Q <= 1; else if (E) Q <= D; endmodule
module \$_SDFF_PP1_ (input C, D, R, output reg Q); always @(posedge C) if (R) Q <= 1; else Q <= D; endmodule
module \$_SDFFE_PP1P_ (input C, D, R, E, output reg Q); always @(posedge C) if (R) Q <= 1; else if (E) Q <= D; endmodule
module \$_DFFE_NP_ (input C, D, E, output reg Q); always @(negedge C) if (E) Q <= D; endmodule
module \$_DFFE_NP0P_ (input C, D, R, E, output reg Q); always @(negedge C or posedge R) if (R) Q <= 0; else if (E) Q <= D; endmodule
module \$_SDFF_NP0_ (input C, D, R, output reg Q); always @(negedge C) if (R) Q <= 0; else Q <= D; endmodule
module \$_SDFFE_NP0P_ (input C, D, R, E, output reg Q); always @(negedge C) if (R) Q <= 0; else if (E) Q <= D; endmodule
module \$_DFFE_NP1P_ (input C, D, R, E, output reg Q); always @(negedge C or posedge R) if (R) Q <= 1; else if (E) Q <= D; endmodule
module \$_SDFF_NP1_ (input C, D, R, output reg Q); always @(negedge C) if (R) Q <= 1; else Q <= D; endmodule
module \$_SDFFE_NP1P_ (input C, D, R, E, output reg Q); always @(negedge C) if (R) Q <= 1; else if (E) Q <= D; endmodule
