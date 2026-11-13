// A small FIFO-like storage: entries written in wclk, one read in rclk when
// the synchronized valid says so. -cdc-sva has no held check on the whole
// storage (another entry may be written while one is read), but keeps it
// on the plain qualified bus next to it.
module cdc_mem(input logic wclk, rclk, rst_n, input logic [7:0] wd, input logic [1:0] wa, ra,
               output logic [7:0] rd, bd);
  logic [3:0][7:0] mem;
  logic [7:0] bus;
  logic v, v1, v2;
  always_ff @(posedge wclk or negedge rst_n)
    if (!rst_n) begin mem <= '0; bus <= '0; v <= 0; end
    else begin mem[wa] <= wd; bus <= wd; v <= ~v; end
  always_ff @(posedge rclk or negedge rst_n)
    if (!rst_n) begin v1 <= 0; v2 <= 0; end else begin v1 <= v; v2 <= v1; end
  always_ff @(posedge rclk) if (v2) begin rd <= mem[ra]; bd <= bus; end
endmodule
