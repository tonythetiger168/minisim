// IEEE 1800-2023 12.7.3 The foreach-loop: one or two dimensions, ascending and
// and a skipped dimension. Descending ranges and break are in 12.7.3-foreach-order.
module t;
  logic [7:0] m [0:3];
  int g [2][3];
  int sum;
  initial begin
    foreach (m[i]) m[i] = i * 2;
    $display("%0d %0d", m[0], m[3]);
    foreach (g[i, j]) g[i][j] = i + j;
    sum = 0;
    foreach (g[i, j]) sum += g[i][j];
    $display("%0d", sum);
    foreach (g[, j]) $write("%0d", j);
    $write("\n");
  end
endmodule
//> 0 6
//> 9
//> 012
