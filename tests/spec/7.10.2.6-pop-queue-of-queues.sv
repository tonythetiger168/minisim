// IEEE 1800-2023 7.10.2.6, 7.10.2.7: pop_front() and pop_back() of a queue
// whose elements are queues return the element queue; it can initialize a
// variable, be assigned, or be read in an expression, and the outer queue
// loses it. Popping an empty queue returns an empty queue.
module top;
//! icarus-skip: Icarus 13 has no queues of queues
  bit [31:0] qq[$][$];
  string     sq[$][$];
  initial begin
    bit [31:0] a[$], d[$];
    a = '{5, 7};
    qq.push_back(a);
    qq.push_back('{1, 2, 3});
    qq.push_back('{9});
    begin
      automatic bit [31:0] f[$] = qq.pop_front();
      $display("front %0d %p left %0d", f.size(), f, qq.size());
    end
    d = qq.pop_back();
    $display("back %p left %0d %p", d, qq.size(), qq[0]);
    d = qq.pop_front();
    $display("last %p left %0d", d, qq.size());
    sq.push_back('{"x", "yz"});
    $display("strings %p %0d", sq.pop_back(), sq.size());
    a = '{4};
    a = qq.pop_front();
    $display("empty %0d", a.size());
  end
endmodule
//> front 2 '{5, 7} left 2
//> back '{9} left 1 '{1, 2, 3}
//> last '{1, 2, 3} left 0
//> strings '{"x", "yz"} 0
//> empty 0
