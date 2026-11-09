// IEEE 1800-2023 7.10.1, 7.10.4: concatenation of queues and elements,
// q[a:b] slices with $ as the last index (out-of-range parts left out), a
// write at index q.size() appends, an index computed from $, and a bounded
// queue [$:2] that holds at most 3 elements (7.10).
//! icarus-skip: Icarus 13 does not support queue concatenation, q[a:$] slices, or $ in an expression
module top;
  int q[$] = '{10, 20, 30, 40};
  int r[$] = '{1, 2};
  byte bq[$:2];
  initial begin
    q = {0, q, r, 9};
    foreach (q[i]) $write("%0d ", q[i]);
    $display("");
    q = q[1:4];
    q = q[1:$];
    $display("%0d: %0d %0d", q.size(), q[0], q[$]);
    q = {q[$], q[0:1]};
    foreach (q[i]) $write("%0d ", q[i]);
    $display("");
    q = q[2:9];
    $display("%0d %0d", q.size(), q[0]);
    q[q.size()] = 42;
    $display("%0d %0d", q.size(), q[$]);
    q[$-1] = 8;
    $display("%0d %0d", q[$-1], q[$]);
    q[5] = 1;
    $display("%0d", q.size());
    bq.push_back(1); bq.push_back(2); bq.push_back(3); bq.push_back(4);
    $display("%0d %0d", bq.size(), bq[$]);
  end
endmodule

//> 0 10 20 30 40 1 2 9 
//> 3: 20 40
//> 40 20 30 
//> 1 30
//> 2 42
//> 8 42
//> 2
//> 3 3
