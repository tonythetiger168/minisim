// The same memory written a whole word at a time.
module lanes(input logic clk, input logic we0, we1, input logic [2:0] addr,
             input logic [15:0] wd, output logic [15:0] rd);
  logic [15:0] bram [0:7];
  always_ff @(posedge clk) begin
    for (int i = 0; i < 8; i++)
      if (addr == 3'(i))
        bram[i] <= {we1 ? wd[15:8] : bram[i][15:8], we0 ? wd[7:0] : bram[i][7:0]};
    rd <= bram[addr];
  end
endmodule
