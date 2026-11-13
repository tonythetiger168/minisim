// IEEE 1800-2023 7.12: array methods on a fixed-size array that is a class
// property, through the handle: ordering methods (7.12.2) write the
// elements back into the object; locators and reductions read them. An
// array declared with any range ([1:4], [4:1]) takes the ordering methods
// and the reductions too.
//! icarus-skip: Icarus does not support array methods on class properties
module t;
  class C;
    int a[4] = '{4, 1, 3, 2};
    byte b[1:3] = '{30, 10, 20};
  endclass
  int m[1:4] = '{40, 10, 30, 20};
  int n[4:1] = '{40, 10, 30, 20};
  initial begin
    C c = new;
    C d = c;
    int q[$];
    c.a.sort();
    $display("%p", d.a);
    c.a.rsort();
    $display("%p", c.a);
    c.a.reverse();
    $display("%p", c.a);
    c.b.sort();
    $display("%p %0d", c.b, c.b[1]);
    c.a.shuffle();
    q = c.a.find with (item > 1);
    q.sort();
    $display("%0d %p", c.a.sum(), q);
    m.sort(); n.rsort();
    $display("%p %0d %p %0d %0d", m, m[1], n, n[4], m.sum());
  end
endmodule
//> '{1, 2, 3, 4}
//> '{4, 3, 2, 1}
//> '{1, 2, 3, 4}
//> '{10, 20, 30} 10
//> 10 '{2, 3, 4}
//> '{10, 20, 30, 40} 10 '{40, 30, 20, 10} 40 100
