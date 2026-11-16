// IEEE 1800-2023 7.4.6, 7.10.2: a queue that is an element of a fixed-size
// array, named with an index outside the array: a write (push_back) is
// dropped, and a read finds an empty queue, so pop_front gives the default.
//! icarus-skip: Icarus does not support arrays of queues
module t;
  int q[2][$];
  int v;
  initial begin
    for (int i = 0; i < 4; i++) q[i].push_back(i + 10);
    $display("%0d %0d %0d", q[0].size(), q[1].size(), q[1][0]);
    for (int i = 0; i < 4; i++) begin
      v = q[i].pop_front();
      $display("%0d: %0d", i, v);
    end
  end
endmodule
//> 1 1 11
//> 0: 10
//> 1: 11
//> 2: 0
//> 3: 0
