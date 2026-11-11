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
  wire ce = (CEMUX == "1") ? 1'b1 : (CEMUX == "INV") ? ~CE : CE;
  wire val = (REGSET == "SET");
  initial Q = val;
  generate
    if (SRMODE == "ASYNC") begin : g_async
      always @(posedge clk or posedge LSR) if (LSR) Q <= val; else if (ce) Q <= DI;
    end else begin : g_sync
      always @(posedge clk) if (LSR) Q <= val; else if (ce) Q <= DI;
    end
  endgenerate
endmodule
// Two bits of a carry chain: each a LUT4 for the sum's propagate and INIT[3:0] as the generate LUT2.
module CCU2C #(parameter [15:0] INIT0 = 0, INIT1 = 0, parameter INJECT1_0 = "YES", INJECT1_1 = "YES")
              (input CIN, A0, B0, C0, D0, A1, B1, C1, D1, output S0, S1, COUT);
  wire p0, p1;
  LUT4 #(.INIT(INIT0)) l0 (.A(A0), .B(B0), .C(C0), .D(D0), .Z(p0));
  LUT4 #(.INIT(INIT1)) l1 (.A(A1), .B(B1), .C(C1), .D(D1), .Z(p1));
  wire g0 = (INJECT1_0 == "YES") ? 1'b0 : INIT0[{B0, A0}];
  wire g1 = (INJECT1_1 == "YES") ? 1'b0 : INIT1[{B1, A1}];
  wire c0 = (INJECT1_0 == "YES") ? 1'b0 : CIN;
  wire co0 = (~p0 & g0) | (p0 & CIN);
  wire c1 = (INJECT1_1 == "YES") ? 1'b0 : co0;
  assign S0 = p0 ^ c0;
  assign S1 = p1 ^ c1;
  assign COUT = (~p1 & g1) | (p1 & co0);
endmodule
