// Simulation models of the memory cells the memu tests use (tests/emu/mems.txt
// describes the same cells to memu).

// 16 x 8, one port: on a rising CLK with CS, a write when WE, else a read.
module SRAM16X8 (input CLK, input CS, input WE, input [3:0] A, input [7:0] D, output reg [7:0] Q);
  reg [7:0] mem [0:15];
  always @(posedge CLK) if (CS) begin
    if (WE) mem[A] <= D;
    else Q <= mem[A];
  end
endmodule

// 16 x 8 register file: a write on a rising WCLK when WE, and a read with no clock.
module RF16X8 (input WCLK, input WE, input [3:0] WA, input [7:0] D, input [3:0] RA, output [7:0] Q);
  reg [7:0] mem [0:15];
  always @(posedge WCLK) if (WE) mem[WA] <= D;
  assign Q = mem[RA];
endmodule
