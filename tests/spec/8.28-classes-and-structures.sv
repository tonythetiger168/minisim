// IEEE 1800-2023 8.28 / 8.29: Class handles can be stored in arrays, queues,
// associative arrays and structs, and passed to and returned from tasks.
// Objects stay alive while any handle refers to them.
module top;
  class Node;
    int v;
    function new(int x); v = x; endfunction
  endclass
  Node byname[string];
  Node arr[4];
  Node q[$];
  int sum;
  function automatic Node mk(int x); Node n = new(x); return n; endfunction
  initial begin
    for (int i = 0; i < 4; i++) begin
      arr[i] = mk(i + 1);
      q.push_front(arr[i]);
    end
    byname["a"] = arr[2];
    byname["b"] = q[0];
    foreach (q[i]) sum += q[i].v;
    arr[2].v = 30;
    $display("%0d %0d %0d %0d", sum, byname["a"].v, byname["b"].v, byname.num());
  end
endmodule

//> 10 30 4 2
