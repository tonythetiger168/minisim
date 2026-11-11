// A memory written one byte lane at a time: two part-selects of the same word in one block.
module lanes(input logic clk, input logic we0, we1, input logic [2:0] addr,
             input logic [15:0] wd, output logic [15:0] rd);
  logic [15:0] bram [0:7];
  always_ff @(posedge clk) begin
    if (we0) bram[addr][7:0] <= wd[7:0];
`ifdef BUG
    if (we1) bram[addr][15:8] <= wd[7:0];
`else
    if (we1) bram[addr][15:8] <= wd[15:8];
`endif
    rd <= bram[addr];
  end
endmodule
