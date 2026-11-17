// Combinational blocks that read a whole ready vector and each write one bit
// of it (the routers of a mesh, bench/gen_manycore.py): a cycle of blocks
// that settles, no bit depending on itself. msim -compile merges such a
// cycle into one block that runs its members until the bits fed back stop
// changing (msim.cpp mergeCombCycles), each member in its own function.
// The clocked block has a loop with a local variable, bits of a packed array
// at a run-time index, and an array written at fixed and run-time indexes,
// which all keep it in the cycle domain (in program order: mem[0] twice).
module ringcycle;
  logic clk = 0, rst = 1;
  always #5 clk = ~clk;
  logic [3:0] rdy, want, ok;
  for (genvar k = 0; k < 4; k++) begin : g
    always_comb begin
      logic [3:0] r;
      r = rdy;
      if (k == 3) rdy[k] = ok[k];
      else rdy[k] = !want[k] || (r[k + 1] && ok[k]);
    end
  end
  logic [3:0][7:0] q;
  logic [7:0] mem [8];
  logic [2:0] idx;
  always_ff @(posedge clk) begin
    if (rst) begin
      q <= '0;
      idx <= '0;
      for (int i = 0; i < 8; i++) mem[i] <= 8'(i * 3);
    end else begin
      for (int i = 0; i < 4; i++) begin
        logic t;
        t = rdy[i] ^ want[i];
        if (t) q[i][idx] <= ~q[i][idx];
      end
      mem[0] <= mem[0] + 8'd1;
      mem[idx] <= {5'd0, idx} + q[0];
      idx <= idx + 3'd3;
    end
  end
  initial begin
    want = 4'b1011; ok = 4'b1110;
    #12 rst = 0;
    for (int c = 0; c < 24; c++) begin
      @(negedge clk);
      $display("%0t rdy=%b q=%h idx=%0d mem0=%0d mem3=%0d", $time, rdy, q, idx, mem[0], mem[3]);
      want = want ^ 4'(c * 5 + 1);
      ok = {ok[2:0], ok[3] ^ c[1]};
    end
    $finish;
  end
endmodule
