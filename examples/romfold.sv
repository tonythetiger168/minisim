// Parameter arrays read in a function's loops (Keccak's pi step reads
// PiRotate[x][y]) and a parameter-indexed part select: once -compile
// unrolls and inlines the loops, every index is a constant, so the reads
// become constants and the a[i +: w] selects fixed part selects
// (msim.cpp propagateConstants, MSIM_NOROMFOLD).
module romfold;
  localparam int W = 8;
  typedef logic [4:0][4:0][W-1:0] box_t;
  localparam int PiRotate [5][5] = '{
    '{0, 3, 1, 4, 2}, '{1, 4, 2, 0, 3}, '{2, 0, 3, 1, 4}, '{3, 1, 4, 2, 0}, '{4, 2, 0, 3, 1}};
  localparam int Off [5] = '{3, 1, 4, 0, 2};
  function automatic box_t pi(box_t st);
    box_t r;
    for (int x = 0; x < 5; x++)
      for (int y = 0; y < 5; y++)
        r[x][y][W-1:0] = st[PiRotate[x][y]][x][W-1:0];
    return r;
  endfunction
  logic [25*W-1:0] s;
  wire box_t b = pi(s);
  wire [4:0][W-1:0] lane;
  for (genvar k = 0; k < 5; k++) begin : g
    assign lane[k] = s[Off[k]*W +: W];
  end
  logic clk = 0;
  always #5 clk = ~clk;
  int n = 0;
  always @(posedge clk) begin
    s <= {25{n[7:0] * 8'd29}} ^ {25{8'(n)}} << n;
    n <= n + 1;
    $display("%0d %h %h", n, b, lane);
    if (n == 8) $finish;
  end
endmodule
