// Behavioural models of the Xilinx 7-series cells mfpga writes, for the simulation tests.
// They follow the vendor's simulation library for the pins mfpga uses.
module LUT1 #(parameter [1:0] INIT = 0) (input I0, output O);
  assign O = I0 ? INIT[1:1] : INIT[0:0];
endmodule
module LUT2 #(parameter [3:0] INIT = 0) (input I0, I1, output O);
  wire [1:0] s1 = I1 ? INIT[3:2] : INIT[1:0];
  assign O = I0 ? s1[1:1] : s1[0:0];
endmodule
module LUT3 #(parameter [7:0] INIT = 0) (input I0, I1, I2, output O);
  wire [3:0] s2 = I2 ? INIT[7:4] : INIT[3:0];
  wire [1:0] s1 = I1 ? s2[3:2] : s2[1:0];
  assign O = I0 ? s1[1:1] : s1[0:0];
endmodule
module LUT4 #(parameter [15:0] INIT = 0) (input I0, I1, I2, I3, output O);
  wire [7:0] s3 = I3 ? INIT[15:8] : INIT[7:0];
  wire [3:0] s2 = I2 ? s3[7:4] : s3[3:0];
  wire [1:0] s1 = I1 ? s2[3:2] : s2[1:0];
  assign O = I0 ? s1[1:1] : s1[0:0];
endmodule
module LUT5 #(parameter [31:0] INIT = 0) (input I0, I1, I2, I3, I4, output O);
  wire [15:0] s4 = I4 ? INIT[31:16] : INIT[15:0];
  wire [7:0] s3 = I3 ? s4[15:8] : s4[7:0];
  wire [3:0] s2 = I2 ? s3[7:4] : s3[3:0];
  wire [1:0] s1 = I1 ? s2[3:2] : s2[1:0];
  assign O = I0 ? s1[1:1] : s1[0:0];
endmodule
module LUT6 #(parameter [63:0] INIT = 0) (input I0, I1, I2, I3, I4, I5, output O);
  wire [31:0] s5 = I5 ? INIT[63:32] : INIT[31:0];
  wire [15:0] s4 = I4 ? s5[31:16] : s5[15:0];
  wire [7:0] s3 = I3 ? s4[15:8] : s4[7:0];
  wire [3:0] s2 = I2 ? s3[7:4] : s3[3:0];
  wire [1:0] s1 = I1 ? s2[3:2] : s2[1:0];
  assign O = I0 ? s1[1:1] : s1[0:0];
endmodule
module IBUF (input I, output O); assign O = I; endmodule
module OBUF (input I, output O); assign O = I; endmodule
module FDRE #(parameter [0:0] INIT = 0, IS_C_INVERTED = 0) (input C, CE, D, R, output reg Q = INIT);
  wire clk = C ^ IS_C_INVERTED;
  always @(posedge clk) if (R) Q <= 0; else if (CE) Q <= D;
endmodule
module FDCE #(parameter [0:0] INIT = 0, IS_C_INVERTED = 0) (input C, CE, D, CLR, output reg Q = INIT);
  wire clk = C ^ IS_C_INVERTED;
  always @(posedge clk or posedge CLR) if (CLR) Q <= 0; else if (CE) Q <= D;
endmodule
module FDPE #(parameter [0:0] INIT = 0, IS_C_INVERTED = 0) (input C, CE, D, PRE, output reg Q = INIT);
  wire clk = C ^ IS_C_INVERTED;
  always @(posedge clk or posedge PRE) if (PRE) Q <= 1; else if (CE) Q <= D;
endmodule
module FDSE #(parameter [0:0] INIT = 1, IS_C_INVERTED = 0) (input C, CE, D, S, output reg Q = INIT);
  wire clk = C ^ IS_C_INVERTED;
  always @(posedge clk) if (S) Q <= 1; else if (CE) Q <= D;
endmodule
module CARRY4 (input CI, CYINIT, input [3:0] DI, S, output [3:0] O, CO);
  wire c0 = CI | CYINIT;
  assign CO[0] = S[0] ? c0 : DI[0];
  assign CO[1] = S[1] ? CO[0] : DI[1];
  assign CO[2] = S[2] ? CO[1] : DI[2];
  assign CO[3] = S[3] ? CO[2] : DI[3];
  assign O = S ^ {CO[2:0], c0};
endmodule
// The multiply of DSP48E1 as mfpga configures it: P = A[24:0] * B, signed, no registers.
module DSP48E1 #(parameter ACASCREG = 0, ADREG = 0, ALUMODEREG = 0, AREG = 0, BCASCREG = 0, BREG = 0, CARRYINREG = 0,
                 CARRYINSELREG = 0, CREG = 0, DREG = 0, INMODEREG = 0, MREG = 0, OPMODEREG = 0, PREG = 0,
                 A_INPUT = "DIRECT", B_INPUT = "DIRECT", USE_MULT = "MULTIPLY", USE_SIMD = "ONE48", USE_DPORT = "FALSE")
  (input [29:0] A, ACIN, input [17:0] B, BCIN, input [47:0] C, PCIN, input [24:0] D, input CARRYIN,
   input [4:0] INMODE, input [3:0] ALUMODE, input [6:0] OPMODE, input [2:0] CARRYINSEL, output [47:0] P);
  wire [47:0] ea = {{23{A[24]}}, A[24:0]};
  wire [47:0] eb = {{30{B[17]}}, B};
  assign P = ea * eb;
endmodule

// RAMB18E1 as mfpga uses it: port A reads, port B writes, no output registers.
module RAMB18E1 #(parameter RAM_MODE = "TDP", READ_WIDTH_A = 0, READ_WIDTH_B = 0, WRITE_WIDTH_A = 0, WRITE_WIDTH_B = 0,
                  DOA_REG = 0, DOB_REG = 0, WRITE_MODE_A = "WRITE_FIRST", WRITE_MODE_B = "WRITE_FIRST",
                  parameter [0:0] IS_CLKARDCLK_INVERTED = 0, IS_CLKBWRCLK_INVERTED = 0, parameter SIM_DEVICE = "7SERIES")
  (input CLKARDCLK, ENARDEN, REGCEAREGCE, RSTRAMARSTRAM, RSTREGARSTREG, input [1:0] WEA, input [13:0] ADDRARDADDR,
   input [15:0] DIADI, input [1:0] DIPADIP, output [15:0] DOADO, output [1:0] DOPADOP,
   input CLKBWRCLK, ENBWREN, REGCEB, RSTRAMB, RSTREGB, input [3:0] WEBWE, input [13:0] ADDRBWRADDR,
   input [15:0] DIBDI, input [1:0] DIPBDIP, output [15:0] DOBDO, output [1:0] DOPBDOP);
  localparam SA = READ_WIDTH_A == 18 ? 4 : READ_WIDTH_A == 9 ? 3 : READ_WIDTH_A == 4 ? 2 : READ_WIDTH_A == 2 ? 1 : 0;
  localparam SB = WRITE_WIDTH_B == 18 ? 4 : WRITE_WIDTH_B == 9 ? 3 : WRITE_WIDTH_B == 4 ? 2 : WRITE_WIDTH_B == 2 ? 1 : 0;
  // data and parity are separate arrays of bits: 16384 and 2048
  reg [16383:0] mem;
  reg [2047:0] par;
  reg [15:0] doa = 16'd0;
  reg [1:0] dopa = 2'd0;
  wire clka = CLKARDCLK ^ IS_CLKARDCLK_INVERTED;
  wire clkb = CLKBWRCLK ^ IS_CLKBWRCLK_INVERTED;
  wire [13:0] ra = (ADDRARDADDR >> SA) << SA;
  wire [13:0] wa = (ADDRBWRADDR >> SB) << SB;
  localparam ND_A = READ_WIDTH_A >= 9 ? READ_WIDTH_A / 9 * 8 : READ_WIDTH_A;
  localparam ND_B = WRITE_WIDTH_B >= 9 ? WRITE_WIDTH_B / 9 * 8 : WRITE_WIDTH_B;
  integer i;
  always @(posedge clkb) if (ENBWREN) begin
    for (i = 0; i < ND_B; i = i + 1) if (WEBWE[i / 8]) mem[wa + i] <= DIBDI[i];
    for (i = 0; i < WRITE_WIDTH_B - ND_B; i = i + 1) if (WEBWE[i]) par[(wa >> 3) + i] <= DIPBDIP[i];
  end
  always @(posedge clka) if (ENARDEN) begin
    if (RSTRAMARSTRAM) begin doa <= 16'd0; dopa <= 2'd0; end
    // a word read while port B writes it is not defined on two clocks: x here, so a test sees it
    else if (ENBWREN && WEBWE != 4'd0 && wa == ra) begin doa <= 16'hxxxx; dopa <= 2'bxx; end
    else begin
      for (i = 0; i < ND_A; i = i + 1) doa[i] <= mem[ra + i];
      for (i = 0; i < READ_WIDTH_A - ND_A; i = i + 1) dopa[i] <= par[(ra >> 3) + i];
    end
  end
  assign DOADO = doa;
  assign DOPADOP = dopa;
  assign DOBDO = 16'd0;
  assign DOPBDOP = 2'd0;
endmodule
