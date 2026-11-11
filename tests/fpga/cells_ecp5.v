// Behavioural models of the ECP5 cells mfpga writes, for the simulation tests.
// They follow the vendor's simulation library for the pins mfpga uses.
module LUT4 #(parameter [15:0] INIT = 0) (input A, B, C, D, output Z);
  wire [7:0] s3 = D ? INIT[15:8] : INIT[7:0];
  wire [3:0] s2 = C ? s3[7:4] : s3[3:0];
  wire [1:0] s1 = B ? s2[3:2] : s2[1:0];
  assign Z = A ? s1[1:1] : s1[0:0];
endmodule
module TRELLIS_FF #(parameter GSR = "ENABLED", CEMUX = "1", CLKMUX = "CLK", LSRMUX = "LSR",
                    REGSET = "RESET", SRMODE = "LSR_OVER_CE") (input CLK, LSR, CE, DI, output reg Q);
  wire clk = (CLKMUX == "INV") ? ~CLK : CLK;
  wire val = (REGSET == "SET");
  initial Q = val;
  generate
    if (SRMODE == "ASYNC") begin : g_async
      always @(posedge clk or posedge LSR) if (LSR) Q <= val; else Q <= DI;
    end else begin : g_sync
      always @(posedge clk) if (LSR) Q <= val; else Q <= DI;
    end
  endgenerate
endmodule
