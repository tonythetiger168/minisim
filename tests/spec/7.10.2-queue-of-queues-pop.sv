// IEEE 1800-2023 7.10.2.4, 7.10.2.5 pop_front() and pop_back() of a queue
// whose elements are queues: the element comes back as a queue and leaves.
//! icarus-skip: Icarus 13 has no queues of queues
module t;
  int qq[$][$];
  int a[$];
  initial begin
    int d[$], e[$];
    a = '{1, 2};    qq.push_back(a);
    a = '{3, 4, 5}; qq.push_back(a);
    a = '{6};       qq.push_back(a);
    d = qq.pop_front();
    e = qq.pop_back();
    $display("%p %p %0d %p", d, e, qq.size(), qq[0]);
    d = qq.pop_front();
    $display("%p %0d", d, qq.size());
  end
endmodule
//> '{1, 2} '{6} 1 '{3, 4, 5}
//> '{3, 4, 5} 0
