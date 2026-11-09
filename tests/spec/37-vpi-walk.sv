// IEEE 1800-2023 37.10, 37.12-37.17: the VPI object model of a design.
// From the top modules (vpi_iterate(vpiModule, NULL)) down through module
// instances: each one's ports and their directions, nets, regs, integer
// variables, memories (and a word by index), and parameters with values.
//! args: data/vpi/walk.c
//! icarus-vpi: data/vpi/walk.c
module top;
  wire [3:0] a = 4'b1001;
  reg [1:0] b = 2'b10;
  integer n = 3;
  reg [7:0] mem [0:3];
  parameter P = 7;
  leaf #(.W(2)) l1(.i(b), .o());
  leaf l2(.i(a[1:0]), .o());
  initial begin
    mem[2] = 8'ha5;
    #1 $walk;
  end
endmodule
module leaf #(parameter W = 4) (input [W-1:0] i, output [W-1:0] o);
  assign o = ~i;
endmodule
//> module top (top) top top=1
//>   net a size 4 = 1001
//>   reg b size 2 = 10
//>   integer n size 32 = 00000000000000000000000000000011
//>   memory mem size 4 [2]=a5 (top.mem[2])
//>   parameter P size 32 = 7
//>   module top.l1 (leaf) l1 top=0
//>     port i size 2 input
//>     port o size 2 output
//>     net i size 2 = 10
//>     net o size 2 = 01
//>     parameter W size 32 = 2
//>   module top.l2 (leaf) l2 top=0
//>     port i size 4 input
//>     port o size 4 output
//>     net i size 4 = 0001
//>     net o size 4 = 1110
//>     parameter W size 32 = 4
