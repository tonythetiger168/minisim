// Simulation models of the generic cells the scan tests use (msynth's names).
module DFF (D, CK, Q); input D, CK; output reg Q; always @(posedge CK) Q <= D; endmodule
module DFFN (D, CKN, Q); input D, CKN; output reg Q; always @(negedge CKN) Q <= D; endmodule
module DFFR (D, CK, RN, Q); input D, CK, RN; output reg Q;
  always @(posedge CK or negedge RN) if (!RN) Q <= 1'b0; else Q <= D;
endmodule
module DFFS (D, CK, SN, Q); input D, CK, SN; output reg Q;
  always @(posedge CK or negedge SN) if (!SN) Q <= 1'b1; else Q <= D;
endmodule
module SDFF (D, SI, SE, CK, Q); input D, SI, SE, CK; output reg Q;
  always @(posedge CK) Q <= SE ? SI : D;
endmodule
module SDFFR (D, SI, SE, CK, RN, Q); input D, SI, SE, CK, RN; output reg Q;
  always @(posedge CK or negedge RN) if (!RN) Q <= 1'b0; else Q <= SE ? SI : D;
endmodule
module MUX2 (A, B, S, Y); input A, B, S; output Y; assign Y = S ? B : A; endmodule
module BUF (A, Y); input A; output Y; assign Y = A; endmodule
module INV (A, Y); input A; output Y; assign Y = ~A; endmodule
module XOR2 (A, B, Y); input A, B; output Y; assign Y = A ^ B; endmodule
module AND2 (A, B, Y); input A, B; output Y; assign Y = A & B; endmodule
module ICG (CK, E, TE, GCK); input CK, E, TE; output GCK; reg en;
  always @(CK or E or TE) if (!CK) en <= E | TE;
  assign GCK = CK & en;
endmodule
module LATCH (D, G, Q); input D, G; output reg Q; always @(G or D) if (G) Q <= D; endmodule
module LATCHN (D, GN, Q); input D, GN; output reg Q; always @(GN or D) if (!GN) Q <= D; endmodule
