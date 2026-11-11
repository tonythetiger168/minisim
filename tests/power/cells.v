// Simulation models of the cells in pw.lib.
`timescale 1ns/1ps
module INV(input A, output Y); assign Y = ~A; endmodule
module BUF(input A, output Y); assign Y = A; endmodule
module NAND2(input A, input B, output Y); assign Y = ~(A & B); endmodule
module XOR2(input A, input B, output Y); assign Y = A ^ B; endmodule
module DFF(input D, input CK, output reg Q); always @(posedge CK) Q <= D; endmodule
module CKGATE(input EN, input CK, output GCK);
  reg q;
  always @(CK or EN) if (!CK) q = EN;
  assign GCK = CK & q;
endmodule
