// The memory cells as black boxes, for synthesis.
(* blackbox *) module SRAM16X8 (input CLK, input CS, input WE, input [3:0] A, input [7:0] D, output [7:0] Q); endmodule
(* blackbox *) module RF16X8 (input WCLK, input WE, input [3:0] WA, input [7:0] D, input [3:0] RA, output [7:0] Q); endmodule
