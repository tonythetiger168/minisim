// IEEE 1800-2023 7.10: queues. push/pop at both ends, insert, delete,
// $ as the last index, initializers, and foreach.
module top;
  int q[$];
  int r[$] = '{1, 2, 3};
  int x;
  initial begin
    $display("%0d", q.size());
    q.push_back(10);
    q.push_back(20);
    q.push_front(5);
    $display("%0d: %0d %0d %0d", q.size(), q[0], q[1], q[$]);
    q.insert(1, 7);
    $display("%0d %0d %0d %0d", q[0], q[1], q[2], q[3]);
    x = q.pop_front();
    $display("%0d %0d", x, q.size());
    x = q.pop_back();
    $display("%0d %0d %0d", x, q.size(), q[$]);
    q.delete(0);
    $display("%0d %0d", q.size(), q[0]);
    foreach (r[i]) q.push_back(r[i]);
    foreach (q[i]) $write("%0d ", q[i]);
    $display("");
    q = {};
    $display("%0d", q.size());
    $display("%0d", r.size());
  end
endmodule

//> 0
//> 3: 5 10 20
//> 5 7 10 20
//> 5 3
//> 20 2 10
//> 1 10
//> 10 1 2 3 
//> 0
//> 3
