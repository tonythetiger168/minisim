// msim -compile: runs of one-bit writes of one register (generate loops,
// always_comb for loops) as one word in the 2-state code (bitRun2): a bit
// reverse, bits moved by a distance, constant bits, a one-hot decoder, and
// the same inside if / case blocks.
module bitrun;
  logic clk = 0;
  logic [31:0] a = 32'h1234_5678, rev, mov, dec, mix, blk;
  logic [4:0] idx = 0;
  logic en = 1, sel = 0;
  logic [31:0] acc = 0;
  for (genvar k = 0; k < 32; k++) begin : g
    assign rev[k] = a[31 - k];
  end
  always_comb begin
    for (int k = 0; k < 24; k++) mov[k + 8] = a[k];
    for (int k = 0; k < 8; k++) mov[k] = 1'b0;
  end
  always_comb for (int k = 0; k < 32; k++) dec[k] = (idx == k) ? en : 1'b0;
  always_comb begin
    mix = '0;
    for (int k = 0; k < 4; k++) mix[k] = 1'b1;
    for (int k = 4; k < 20; k++) mix[k] = a[k + 3];
    mix[20] = en; mix[21] = sel; mix[22] = a[0] ^ a[1];
  end
  always_comb begin
    if (sel) begin
      for (int k = 0; k < 16; k++) blk[k] = a[15 - k];
      for (int k = 16; k < 32; k++) blk[k] = 1'b0;
    end else begin
      for (int k = 0; k < 32; k++) blk[k] = (idx == k) ? sel ^ en : 1'b0;
    end
  end
  always @(posedge clk) begin
    a <= a * 1103515245 + 12345 + rev[7:0];
    idx <= idx + 3 + a[1];
    en <= ~en | a[2];
    sel <= a[3];
    acc <= acc * 31 + rev + mov + dec + mix + blk;
  end
  initial begin
    repeat (400) begin #5 clk = 1; #5 clk = 0; end
    $display("acc=%h a=%h rev=%h dec=%h blk=%h", acc, a, rev, dec, blk); $finish;
  end
endmodule
