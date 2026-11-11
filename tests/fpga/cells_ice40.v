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
// SB_RAM40_4K and its falling-edge variants, without initial contents (one clock assumed for
// the clash check): 4096 bits read and
// written 16, 8, 4 or 2 at a time (READ_MODE / WRITE_MODE 0..3) on the data bits each mode uses.
module SB_RAM40_4K #(parameter READ_MODE = 0, WRITE_MODE = 0)
  (output reg [15:0] RDATA, input RCLK, RCLKE, RE, input [10:0] RADDR,
   input WCLK, WCLKE, WE, input [10:0] WADDR, input [15:0] MASK, WDATA);
  reg [4095:0] mem;
  integer i;
  always @(posedge WCLK) if (WE && WCLKE) begin
    case (WRITE_MODE)
      0: for (i = 0; i < 16; i = i + 1) if (!MASK[i]) mem[{WADDR[7:0], 4'd0} + i] <= WDATA[i];
      1: for (i = 0; i < 8; i = i + 1) mem[{WADDR[8:0], 3'd0} + i] <= WDATA[2 * i];
      2: for (i = 0; i < 4; i = i + 1) mem[{WADDR[9:0], 2'd0} + i] <= WDATA[4 * i + 1];
      default: for (i = 0; i < 2; i = i + 1) mem[{WADDR[10:0], 1'd0} + i] <= WDATA[8 * i + 3];
    endcase
  end
  // a word read while it is written is not defined in the block: x here, so a test sees it
  wire [10:0] used = READ_MODE == 0 ? 11'h0ff : READ_MODE == 1 ? 11'h1ff : READ_MODE == 2 ? 11'h3ff : 11'h7ff;
  wire clash = WE && WCLKE && ((WADDR ^ RADDR) & used) == 11'd0;
  always @(posedge RCLK) if (RE && RCLKE) begin
    RDATA <= 16'd0;
    if (clash) RDATA <= 16'hxxxx;
    else case (READ_MODE)
      0: for (i = 0; i < 16; i = i + 1) RDATA[i] <= mem[{RADDR[7:0], 4'd0} + i];
      1: for (i = 0; i < 8; i = i + 1) RDATA[2 * i] <= mem[{RADDR[8:0], 3'd0} + i];
      2: for (i = 0; i < 4; i = i + 1) RDATA[4 * i + 1] <= mem[{RADDR[9:0], 2'd0} + i];
      default: for (i = 0; i < 2; i = i + 1) RDATA[8 * i + 3] <= mem[{RADDR[10:0], 1'd0} + i];
    endcase
  end
endmodule
module SB_RAM40_4KNR #(parameter READ_MODE = 0, WRITE_MODE = 0)
  (output [15:0] RDATA, input RCLKN, RCLKE, RE, input [10:0] RADDR,
   input WCLK, WCLKE, WE, input [10:0] WADDR, input [15:0] MASK, WDATA);
  SB_RAM40_4K #(.READ_MODE(READ_MODE), .WRITE_MODE(WRITE_MODE)) r (.RDATA(RDATA), .RCLK(~RCLKN), .RCLKE(RCLKE), .RE(RE),
    .RADDR(RADDR), .WCLK(WCLK), .WCLKE(WCLKE), .WE(WE), .WADDR(WADDR), .MASK(MASK), .WDATA(WDATA));
endmodule
module SB_RAM40_4KNW #(parameter READ_MODE = 0, WRITE_MODE = 0)
  (output [15:0] RDATA, input RCLK, RCLKE, RE, input [10:0] RADDR,
   input WCLKN, WCLKE, WE, input [10:0] WADDR, input [15:0] MASK, WDATA);
  SB_RAM40_4K #(.READ_MODE(READ_MODE), .WRITE_MODE(WRITE_MODE)) r (.RDATA(RDATA), .RCLK(RCLK), .RCLKE(RCLKE), .RE(RE),
    .RADDR(RADDR), .WCLK(~WCLKN), .WCLKE(WCLKE), .WE(WE), .WADDR(WADDR), .MASK(MASK), .WDATA(WDATA));
endmodule
module SB_RAM40_4KNRNW #(parameter READ_MODE = 0, WRITE_MODE = 0)
  (output [15:0] RDATA, input RCLKN, RCLKE, RE, input [10:0] RADDR,
   input WCLKN, WCLKE, WE, input [10:0] WADDR, input [15:0] MASK, WDATA);
  SB_RAM40_4K #(.READ_MODE(READ_MODE), .WRITE_MODE(WRITE_MODE)) r (.RDATA(RDATA), .RCLK(~RCLKN), .RCLKE(RCLKE), .RE(RE),
    .RADDR(RADDR), .WCLK(~WCLKN), .WCLKE(WCLKE), .WE(WE), .WADDR(WADDR), .MASK(MASK), .WDATA(WDATA));
endmodule
