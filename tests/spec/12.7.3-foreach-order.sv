// IEEE 1800-2023 12.7.3 The foreach-loop: each index runs from the left bound to
// the right bound, so [3:1] counts down (the clause's own example: q from 5 to 1).
// With 12.8: break leaves the whole foreach, continue goes to the next index.
//! icarus-skip: Icarus 12 counts [3:1] upward, and reports "sorry: break statements not supported"
module t;
  int g [2][3];
  logic [7:0] d [3:1];
  initial begin
    foreach (d[i]) $write("%0d ", i);
    $write("\n");
    foreach (g[i, j]) begin
      if (j == 1) continue;
      $write("%0d%0d ", i, j);
    end
    $write("\n");
    foreach (g[i, j]) begin
      if (i == 1) break;
      $write("%0d%0d ", i, j);
    end
    $write("\n");
  end
endmodule
//> 3 2 1 
//> 00 02 10 12 
//> 00 01 02 
