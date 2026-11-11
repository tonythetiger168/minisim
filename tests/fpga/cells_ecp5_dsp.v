// The combinational multiply of MULT18X18D (no registers), for the mfpga tests.
// Yosys 0.33 has this cell as a black box only, so the proof uses this model too.
module MULT18X18D #(parameter REG_INPUTA_CLK = "NONE", REG_INPUTB_CLK = "NONE", REG_INPUTC_CLK = "NONE",
                    REG_PIPELINE_CLK = "NONE", REG_OUTPUT_CLK = "NONE", SOURCEB_MODE = "B_SHIFT",
                    MULT_BYPASS = "DISABLED", RESETMODE = "SYNC", GSR = "ENABLED")
  (input A0, A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15, A16, A17,
   input B0, B1, B2, B3, B4, B5, B6, B7, B8, B9, B10, B11, B12, B13, B14, B15, B16, B17,
   input C0, C1, C2, C3, C4, C5, C6, C7, C8, C9, C10, C11, C12, C13, C14, C15, C16, C17,
   input SIGNEDA, SIGNEDB, SOURCEA, SOURCEB,
   output P0, P1, P2, P3, P4, P5, P6, P7, P8, P9, P10, P11, P12, P13, P14, P15, P16, P17, P18, P19, P20, P21, P22, P23, P24, P25, P26, P27, P28, P29, P30, P31, P32, P33, P34, P35);
  wire [17:0] a = {A17, A16, A15, A14, A13, A12, A11, A10, A9, A8, A7, A6, A5, A4, A3, A2, A1, A0};
  wire [17:0] b = {B17, B16, B15, B14, B13, B12, B11, B10, B9, B8, B7, B6, B5, B4, B3, B2, B1, B0};
  wire [35:0] ea = {{18{SIGNEDA & a[17]}}, a};
  wire [35:0] eb = {{18{SIGNEDB & b[17]}}, b};
  assign {P35, P34, P33, P32, P31, P30, P29, P28, P27, P26, P25, P24, P23, P22, P21, P20, P19, P18, P17, P16, P15, P14, P13, P12, P11, P10, P9, P8, P7, P6, P5, P4, P3, P2, P1, P0} = ea * eb;
endmodule
// DP16KD as mfpga uses it: port A writes, port B reads, no output register, no initial
// contents, chip selects 0. 16384 bits in words of DATA_WIDTH; the address is the top of AD[13:0].
module DP16KD #(parameter DATA_WIDTH_A = 18, DATA_WIDTH_B = 18, REGMODE_A = "NOREG", REGMODE_B = "NOREG",
                RESETMODE = "SYNC", ASYNC_RESET_RELEASE = "SYNC", CSDECODE_A = "0b000", CSDECODE_B = "0b000",
                WRITEMODE_A = "NORMAL", WRITEMODE_B = "NORMAL", CLKAMUX = "CLKA", CLKBMUX = "CLKB", GSR = "DISABLED")
  (input DIA0, DIA1, DIA2, DIA3, DIA4, DIA5, DIA6, DIA7, DIA8, DIA9, DIA10, DIA11, DIA12, DIA13, DIA14, DIA15, DIA16, DIA17,
   input ADA0, ADA1, ADA2, ADA3, ADA4, ADA5, ADA6, ADA7, ADA8, ADA9, ADA10, ADA11, ADA12, ADA13,
   input CEA, OCEA, CLKA, WEA, RSTA, CSA0, CSA1, CSA2,
   output DOA0, DOA1, DOA2, DOA3, DOA4, DOA5, DOA6, DOA7, DOA8, DOA9, DOA10, DOA11, DOA12, DOA13, DOA14, DOA15, DOA16, DOA17,
   input DIB0, DIB1, DIB2, DIB3, DIB4, DIB5, DIB6, DIB7, DIB8, DIB9, DIB10, DIB11, DIB12, DIB13, DIB14, DIB15, DIB16, DIB17,
   input ADB0, ADB1, ADB2, ADB3, ADB4, ADB5, ADB6, ADB7, ADB8, ADB9, ADB10, ADB11, ADB12, ADB13,
   input CEB, OCEB, CLKB, WEB, RSTB, CSB0, CSB1, CSB2,
   output DOB0, DOB1, DOB2, DOB3, DOB4, DOB5, DOB6, DOB7, DOB8, DOB9, DOB10, DOB11, DOB12, DOB13, DOB14, DOB15, DOB16, DOB17);
  localparam SA = DATA_WIDTH_A == 18 ? 4 : DATA_WIDTH_A == 9 ? 3 : DATA_WIDTH_A == 4 ? 2 : DATA_WIDTH_A == 2 ? 1 : 0;
  localparam SB = DATA_WIDTH_B == 18 ? 4 : DATA_WIDTH_B == 9 ? 3 : DATA_WIDTH_B == 4 ? 2 : DATA_WIDTH_B == 2 ? 1 : 0;
  // a word of 18 or 9 takes 16 or 8 address positions: bit address = word address * 2^S, plus 9 for the high byte
  reg [18431:0] mem;
  wire [13:0] ada = {ADA13, ADA12, ADA11, ADA10, ADA9, ADA8, ADA7, ADA6, ADA5, ADA4, ADA3, ADA2, ADA1, ADA0};
  wire [13:0] adb = {ADB13, ADB12, ADB11, ADB10, ADB9, ADB8, ADB7, ADB6, ADB5, ADB4, ADB3, ADB2, ADB1, ADB0};
  wire [17:0] dia = {DIA17, DIA16, DIA15, DIA14, DIA13, DIA12, DIA11, DIA10, DIA9, DIA8, DIA7, DIA6, DIA5, DIA4, DIA3, DIA2, DIA1, DIA0};
  reg [17:0] dob = 18'd0;
  wire clka = (CLKAMUX == "INV") ? ~CLKA : CLKA;
  wire clkb = (CLKBMUX == "INV") ? ~CLKB : CLKB;
  wire [14:0] wbase = (ada >> SA) * DATA_WIDTH_A;
  wire [14:0] rbase = (adb >> SB) * DATA_WIDTH_B;
  integer i;
  always @(posedge clka) if (CEA && WEA)
    for (i = 0; i < DATA_WIDTH_A; i = i + 1)
      if (DATA_WIDTH_A < 9 || (i < 9 ? ada[0] : ada[1])) mem[wbase + i] <= dia[i];
  always @(posedge clkb) if (CEB) begin
    if (RSTB) dob <= 18'd0;
    // a word read while port A writes it is not defined in the block: x here, so a test sees it
    else if (CEA && WEA && wbase == rbase) dob <= 18'hxxxxx;
    else if (!WEB) for (i = 0; i < DATA_WIDTH_B; i = i + 1) dob[i] <= mem[rbase + i];
  end
  assign {DOB17, DOB16, DOB15, DOB14, DOB13, DOB12, DOB11, DOB10, DOB9, DOB8, DOB7, DOB6, DOB5, DOB4, DOB3, DOB2, DOB1, DOB0} = dob;
  assign {DOA17, DOA16, DOA15, DOA14, DOA13, DOA12, DOA11, DOA10, DOA9, DOA8, DOA7, DOA6, DOA5, DOA4, DOA3, DOA2, DOA1, DOA0} = 18'd0;
endmodule
