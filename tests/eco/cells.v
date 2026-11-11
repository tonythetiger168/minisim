// Verilog models of the cells the ECO tests use (tests/sta/sta_test.lib and
// eco_cells.lib), so that msim can simulate a patched netlist against the new one.
module INV_X1(input A, output Y); assign Y = ~A; endmodule
module BUF_X1(input A, output Y); assign Y = A; endmodule
module NAND2_X1(input A, B, output Y); assign Y = ~(A & B); endmodule
module NOR2_X1(input A, B, output Y); assign Y = ~(A | B); endmodule
module AND2_X1(input A, B, output Y); assign Y = A & B; endmodule
module OR2_X1(input A, B, output Y); assign Y = A | B; endmodule
module XOR2_X1(input A, B, output Y); assign Y = A ^ B; endmodule
module MUX2_X1(input A, B, S, output Y); assign Y = S ? B : A; endmodule
module AOI21_X1(input A1, A2, B, output Y); assign Y = ~((A1 & A2) | B); endmodule
module DFF_X1(input CK, D, output reg Q); initial Q = 0; always @(posedge CK) Q <= D; endmodule
module DFFR_X1(input CK, D, RN, output reg Q); initial Q = 0; always @(posedge CK or negedge RN) if (!RN) Q <= 0; else Q <= D; endmodule
module SDFF_X1(input CK, D, SI, SE, output reg Q); initial Q = 0; always @(posedge CK) Q <= SE ? SI : D; endmodule
module SDFFR_X1(input CK, D, SI, SE, RN, output reg Q); initial Q = 0; always @(posedge CK or negedge RN) if (!RN) Q <= 0; else Q <= SE ? SI : D; endmodule
module DFFQN_X1(input CK, D, output reg Q, output QN); initial Q = 0; assign QN = ~Q; always @(posedge CK) Q <= D; endmodule
module DFFE_X1(input CK, D, E, output reg Q); initial Q = 0; always @(posedge CK) if (E) Q <= D; endmodule
