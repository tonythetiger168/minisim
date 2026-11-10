// Behavioural models of a tiny cell library, for the gate-level tests.
module INV (input A, output Y); assign Y = ~A; endmodule
module BUF (input A, output Y); assign Y = A; endmodule
module AND2 (input A, B, output Y); assign Y = A & B; endmodule
module OR2 (input A, B, output Y); assign Y = A | B; endmodule
module NAND2 (input A, B, output Y); assign Y = ~(A & B); endmodule
module NOR2 (input A, B, output Y); assign Y = ~(A | B); endmodule
module XOR2 (input A, B, output Y); assign Y = A ^ B; endmodule
module XNOR2 (input A, B, output Y); assign Y = ~(A ^ B); endmodule
module MUX2 (input A, B, S, output Y); assign Y = S ? B : A; endmodule
module AOI21 (input A, B, C, output Y); assign Y = ~((A & B) | C); endmodule
module OAI21 (input A, B, C, output Y); assign Y = ~((A | B) & C); endmodule
module TIE0 (output Y); assign Y = 1'b0; endmodule
module TIE1 (output Y); assign Y = 1'b1; endmodule
module DFF (input D, CK, output reg Q);
    always @(posedge CK) Q <= D;
endmodule
module DFFR (input D, CK, RN, output reg Q);
    always @(posedge CK or negedge RN)
        if (!RN) Q <= 1'b0;
        else Q <= D;
endmodule
module DFFS (input D, CK, SN, output reg Q);
    always @(posedge CK or negedge SN)
        if (!SN) Q <= 1'b1;
        else Q <= D;
endmodule
module LATCH (input D, G, output reg Q);
    always @(D or G)
        if (G) Q = D;
endmodule
