// Verilog models of the cells in tiny.lib, for replaying patterns.
module ND2X1 (input A1, input A2, output ZN); assign ZN = ~(A1 & A2); endmodule
module INVX1 (input I, output ZN); assign ZN = ~I; endmodule
module FAX1 (input A, input B, input CI, output S, output CO);
  assign S = A ^ B ^ CI;
  assign CO = (A & B) | (A & CI) | (B & CI);
endmodule
module MX2X1 (input I0, input I1, input S, output Z); assign Z = S ? I1 : I0; endmodule
module SDFRQX1 (input D, input TI, input TE, input CP, input RDN, output reg Q, output QN);
  assign QN = ~Q;
  always @(posedge CP or negedge RDN)
    if (!RDN) Q <= 1'b0;
    else Q <= TE ? TI : D;
endmodule
