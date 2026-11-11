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
// The enable gates the synchronous reset too (SB_DFFESR), as in the vendor's models.
module SB_DFFE (input C, D, E, output reg Q = 0); always @(posedge C) if (E) Q <= D; endmodule
module SB_DFFER (input C, D, E, R, output reg Q = 0); always @(posedge C or posedge R) if (R) Q <= 0; else if (E) Q <= D; endmodule
module SB_DFFSR (input C, D, R, output reg Q = 0); always @(posedge C) if (R) Q <= 0; else Q <= D; endmodule
module SB_DFFESR (input C, D, E, R, output reg Q = 0); always @(posedge C) if (E) begin if (R) Q <= 0; else Q <= D; end endmodule
module SB_DFFES (input C, D, E, S, output reg Q = 0); always @(posedge C or posedge S) if (S) Q <= 1; else if (E) Q <= D; endmodule
module SB_DFFSS (input C, D, S, output reg Q = 0); always @(posedge C) if (S) Q <= 1; else Q <= D; endmodule
module SB_DFFESS (input C, D, E, S, output reg Q = 0); always @(posedge C) if (E) begin if (S) Q <= 1; else Q <= D; end endmodule
module SB_DFFNE (input C, D, E, output reg Q = 0); always @(negedge C) if (E) Q <= D; endmodule
module SB_DFFNER (input C, D, E, R, output reg Q = 0); always @(negedge C or posedge R) if (R) Q <= 0; else if (E) Q <= D; endmodule
module SB_DFFNSR (input C, D, R, output reg Q = 0); always @(negedge C) if (R) Q <= 0; else Q <= D; endmodule
module SB_DFFNESR (input C, D, E, R, output reg Q = 0); always @(negedge C) if (E) begin if (R) Q <= 0; else Q <= D; end endmodule
module SB_DFFNES (input C, D, E, S, output reg Q = 0); always @(negedge C or posedge S) if (S) Q <= 1; else if (E) Q <= D; endmodule
module SB_DFFNSS (input C, D, S, output reg Q = 0); always @(negedge C) if (S) Q <= 1; else Q <= D; endmodule
module SB_DFFNESS (input C, D, E, S, output reg Q = 0); always @(negedge C) if (E) begin if (S) Q <= 1; else Q <= D; end endmodule
module SB_CARRY (input CI, I0, I1, output CO); assign CO = (I0 & I1) | ((I0 | I1) & CI); endmodule
// The 16 x 16 multiply of SB_MAC16 as mfpga configures it (no registers, no accumulator).
module SB_MAC16 #(parameter NEG_TRIGGER = 0, C_REG = 0, A_REG = 0, B_REG = 0, D_REG = 0, TOP_8x8_MULT_REG = 0,
                  BOT_8x8_MULT_REG = 0, PIPELINE_16x16_MULT_REG1 = 0, PIPELINE_16x16_MULT_REG2 = 0,
                  TOPOUTPUT_SELECT = 0, TOPADDSUB_LOWERINPUT = 0, TOPADDSUB_UPPERINPUT = 0, TOPADDSUB_CARRYSELECT = 0,
                  BOTOUTPUT_SELECT = 0, BOTADDSUB_LOWERINPUT = 0, BOTADDSUB_UPPERINPUT = 0, BOTADDSUB_CARRYSELECT = 0,
                  MODE_8x8 = 0, A_SIGNED = 0, B_SIGNED = 0) (input [15:0] A, B, output [31:0] O);
  wire [31:0] ea = {{16{A_SIGNED != 0 && A[15]}}, A};
  wire [31:0] eb = {{16{B_SIGNED != 0 && B[15]}}, B};
  assign O = ea * eb;
endmodule
