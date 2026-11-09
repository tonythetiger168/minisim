// IEEE 1800-2023 7.12.2, 7.12.3: ordering methods (reverse, sort, rsort)
// and reduction methods (sum, product) on queues and dynamic arrays.
//! icarus-skip: Icarus 13 has no sum/product methods and no sort()
module top;
  int q[$] = '{3, 1, 4, 1, 5, 9, 2, 6};
  int d[];
  initial begin
    $display("%0d %0d", q.sum(), q.product());
    q.sort();
    foreach (q[i]) $write("%0d ", q[i]);
    $display("");
    q.rsort();
    foreach (q[i]) $write("%0d ", q[i]);
    $display("");
    q.reverse();
    foreach (q[i]) $write("%0d ", q[i]);
    $display("");
    d = new[4];
    foreach (d[i]) d[i] = i + 1;
    $display("%0d %0d", d.sum(), d.product());
    d.shuffle();
    $display("%0d %0d", d.size(), d.sum());
  end
endmodule

//> 31 6480
//> 1 1 2 3 4 5 6 9 
//> 9 6 5 4 3 2 1 1 
//> 1 1 2 3 4 5 6 9 
//> 10 24
//> 4 10
