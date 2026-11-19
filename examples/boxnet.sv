// A net driven by a function with nested loops (a SHA-3 state reshaped
// bit by bit, as OpenTitan's KMAC does): -compile makes the assign a plain
// write (msim.cpp mergePartialDrivers), unrolls the function's loops and
// inlines it into a comb block (unrollSeqLoops subsOnly, inlineCombCalls),
// and writes each bit of the wide local in place (Sim::setBitsV), also
// x bits before the state is known.
module boxnet;
  localparam int W = 16;
  typedef logic [4:0][4:0][W-1:0] box_t;
  function automatic box_t to_box(logic [25*W-1:0] in);
    box_t b;
    for (int y = 0; y < 5; y++)
      for (int x = 0; x < 5; x++)
        for (int z = 0; z < W; z++)
          b[x][y][z] = in[W*(5*y + x) + z];
    return b;
  endfunction
  logic [25*W-1:0] st;
  wire box_t bx;
  assign bx = to_box(st);
  logic clk = 0;
  always #5 clk = ~clk;
  int n = 0;
  initial #1 $display("x: %h", bx[1][2]);
  always @(posedge clk) begin
    st <= {25{n[7:0] * 8'd37, 8'(n)}};
    st[3] <= 1'bx;
    n <= n + 1;
    $display("%0d %h %h %h", n, bx[0][0], bx[3][2], bx[4][4]);
    if (n == 12) $finish;
  end
endmodule
