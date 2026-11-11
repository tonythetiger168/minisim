// Verilog models of the generic cells (INV ... SDFFRS), for replaying patterns in a simulator.
module INV (input A, output Y);
  assign Y = ~A;
endmodule
module BUF (input A, output Y);
  assign Y = A;
endmodule
module AND2 (input A, input B, output Y);
  assign Y = A & B;
endmodule
module NAND2 (input A, input B, output Y);
  assign Y = ~(A & B);
endmodule
module OR2 (input A, input B, output Y);
  assign Y = A | B;
endmodule
module NOR2 (input A, input B, output Y);
  assign Y = ~(A | B);
endmodule
module AND3 (input A, input B, input C, output Y);
  assign Y = A & B & C;
endmodule
module NAND3 (input A, input B, input C, output Y);
  assign Y = ~(A & B & C);
endmodule
module OR3 (input A, input B, input C, output Y);
  assign Y = A | B | C;
endmodule
module NOR3 (input A, input B, input C, output Y);
  assign Y = ~(A | B | C);
endmodule
module AND4 (input A, input B, input C, input D, output Y);
  assign Y = A & B & C & D;
endmodule
module NAND4 (input A, input B, input C, input D, output Y);
  assign Y = ~(A & B & C & D);
endmodule
module OR4 (input A, input B, input C, input D, output Y);
  assign Y = A | B | C | D;
endmodule
module NOR4 (input A, input B, input C, input D, output Y);
  assign Y = ~(A | B | C | D);
endmodule
module XOR2 (input A, input B, output Y);
  assign Y = A ^ B;
endmodule
module XNOR2 (input A, input B, output Y);
  assign Y = ~(A ^ B);
endmodule
module MUX2 (input A, input B, input S, output Y);
  assign Y = S ? B : A;
endmodule
module AOI21 (input A, input B, input C, output Y);
  assign Y = ~((A & B) | C);
endmodule
module OAI21 (input A, input B, input C, output Y);
  assign Y = ~((A | B) & C);
endmodule
module AOI22 (input A, input B, input C, input D, output Y);
  assign Y = ~((A & B) | (C & D));
endmodule
module OAI22 (input A, input B, input C, input D, output Y);
  assign Y = ~((A | B) & (C | D));
endmodule
module TIE0 (output Y);
  assign Y = 1'b0;
endmodule
module TIE1 (output Y);
  assign Y = 1'b1;
endmodule
module DFF (input D, input CK, output reg Q, output QN);
  assign QN = ~Q;
  always @(posedge CK)
    Q <= D;
endmodule
module DFFS (input D, input CK, input SN, output reg Q, output QN);
  assign QN = ~Q;
  always @(posedge CK or negedge SN)
    if (!SN) Q <= 1'b1;
    else Q <= D;
endmodule
module DFFR (input D, input CK, input RN, output reg Q, output QN);
  assign QN = ~Q;
  always @(posedge CK or negedge RN)
    if (!RN) Q <= 1'b0;
    else Q <= D;
endmodule
module DFFRS (input D, input CK, input RN, input SN, output reg Q, output QN);
  assign QN = ~Q;
  always @(posedge CK or negedge RN or negedge SN)
    if (!RN) Q <= 1'b0;
    else if (!SN) Q <= 1'b1;
    else Q <= D;
endmodule
module SDFF (input D, input SI, input SE, input CK, output reg Q, output QN);
  assign QN = ~Q;
  always @(posedge CK)
    Q <= SE ? SI : D;
endmodule
module SDFFS (input D, input SI, input SE, input CK, input SN, output reg Q, output QN);
  assign QN = ~Q;
  always @(posedge CK or negedge SN)
    if (!SN) Q <= 1'b1;
    else Q <= SE ? SI : D;
endmodule
module SDFFR (input D, input SI, input SE, input CK, input RN, output reg Q, output QN);
  assign QN = ~Q;
  always @(posedge CK or negedge RN)
    if (!RN) Q <= 1'b0;
    else Q <= SE ? SI : D;
endmodule
module SDFFRS (input D, input SI, input SE, input CK, input RN, input SN, output reg Q, output QN);
  assign QN = ~Q;
  always @(posedge CK or negedge RN or negedge SN)
    if (!RN) Q <= 1'b0;
    else if (!SN) Q <= 1'b1;
    else Q <= SE ? SI : D;
endmodule
