// 7.10.1, 7.6: writing a queue slice. q[i:j] = v puts v's elements in place
// of elements i..j, so the queue is {q[0:i-1], v, q[j+1:$]}.
//! icarus-skip: Icarus aborts on a queue slice write
module top;
  int q[$], r[$];
  string s[$];
  initial begin
    q = '{1, 2, 3, 4, 5};
    q[1:3] = '{20, 30, 40};
    $display("%p", q);
    q[0:1] = '{7, 8};
    $display("%p", q);
    q[4:4] = '{9};
    $display("%p", q);
    r = '{100, 200};
    for (int k = 0; k < 2; k++) begin
      q[k+1:k+2] = r;
      $display("%p size %0d", q, q.size());
    end
    q[1:3] = '{0};   // fewer elements: the queue shrinks
    $display("%p size %0d", q, q.size());
    s = '{"a", "b", "c"};
    s[2:2] = '{"z"};
    $display("%p", s);
  end
endmodule
//> '{1, 20, 30, 40, 5}
//> '{7, 8, 30, 40, 5}
//> '{7, 8, 30, 40, 9}
//> '{7, 100, 200, 40, 9} size 5
//> '{7, 100, 100, 200, 9} size 5
//> '{7, 0, 9} size 3
//> '{"a", "b", "z"}
