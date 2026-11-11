// Behavioural models of the iCE40 cells mfpga writes, for the simulation tests.
// They follow the vendor's simulation library for the pins mfpga uses.
module SB_LUT4 #(parameter [15:0] LUT_INIT = 0) (input I0, I1, I2, I3, output O);
  wire [7:0] s3 = I3 ? LUT_INIT[15:8] : LUT_INIT[7:0];
  wire [3:0] s2 = I2 ? s3[7:4] : s3[3:0];
  wire [1:0] s1 = I1 ? s2[3:2] : s2[1:0];
  assign O = I0 ? s1[1:1] : s1[0:0];
endmodule
module SB_DFF (input C, D, output reg Q = 0); always @(posedge C) Q <= D; endmodule
module SB_DFFR (input C, D, R, output reg Q = 0);
  always @(posedge C or posedge R) if (R) Q <= 0; else Q <= D;
endmodule
module SB_DFFS (input C, D, S, output reg Q = 0);
  always @(posedge C or posedge S) if (S) Q <= 1; else Q <= D;
endmodule
module SB_DFFN (input C, D, output reg Q = 0); always @(negedge C) Q <= D; endmodule
module SB_DFFNR (input C, D, R, output reg Q = 0);
  always @(negedge C or posedge R) if (R) Q <= 0; else Q <= D;
endmodule
module SB_DFFNS (input C, D, S, output reg Q = 0);
  always @(negedge C or posedge S) if (S) Q <= 1; else Q <= D;
endmodule
