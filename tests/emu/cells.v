// Simulation models of the cells in tests/sta/sta_test.lib, for the memu
// tests: the gate netlists here are simulated before and after memu.
module INV_X1 (input A, output Y);          assign Y = ~A;          endmodule
module BUF_X1 (input A, output Y);          assign Y = A;           endmodule
module NAND2_X1 (input A, input B, output Y); assign Y = ~(A & B);  endmodule
module NOR2_X1 (input A, input B, output Y);  assign Y = ~(A | B);  endmodule
module AND2_X1 (input A, input B, output Y);  assign Y = A & B;     endmodule
module OR2_X1 (input A, input B, output Y);   assign Y = A | B;     endmodule
module XOR2_X1 (input A, input B, output Y);  assign Y = A ^ B;     endmodule
module MUX2_X1 (input A, input B, input S, output Y); assign Y = S ? B : A; endmodule
module DFF_X1 (input CK, input D, output reg Q);
  always @(posedge CK) Q <= D;
endmodule
module DFFN_X1 (input CK, input D, output reg Q);
  always @(negedge CK) Q <= D;
endmodule
module DFFR_X1 (input CK, input RN, input D, output reg Q);
  always @(posedge CK or negedge RN) if (!RN) Q <= 1'b0; else Q <= D;
endmodule
module LATCH_X1 (input G, input D, output reg Q);
  always @* if (G) Q = D;
endmodule
