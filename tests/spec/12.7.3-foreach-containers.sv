// IEEE 1800-2023 12.7.3: foreach over a queue (index order) and an associative
// array (key order, 7.9), with break and continue; a '{key: value} pattern (10.9.2).
//! icarus-skip: Icarus 13 does not support associative arrays
module top;
  int aa[int] = '{3: 30, 1: 10};
  int q[$] = '{1, 2, 3, 4, 5};
  initial begin
    foreach (aa[k]) $display("%0d %0d", k, aa[k]);
    foreach (q[i]) begin
      if (q[i] == 2) continue;
      if (q[i] == 4) break;
      $display("q %0d", q[i]);
    end
    foreach (aa[k]) begin if (k == 1) continue; $display("a %0d", k); end
  end
endmodule

//> 1 10
//> 3 30
//> q 1
//> q 3
//> a 3
