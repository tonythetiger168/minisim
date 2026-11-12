// IEEE 1800-2023 7.12: array methods beyond the basics.
//  - a locator result used directly as a value: printed with %p, or given
//    to another method (7.12.1)
//  - min, max and unique on string and real elements and with clauses (7.12.1)
//  - item.index in a with clause (7.12.4)
//  - locator and with methods of an associative array, in key order (7.12.1, 7.12.3)
//  - sort, rsort, reverse and shuffle of a fixed-size array (7.12.2)
//! icarus-skip: Icarus 13 has no locator methods
module top;
  int q[$] = '{3, 1, 2, 3, 5};
  string s[$] = '{"pear", "apple", "fig", "apple"};
  real r[$] = '{2.5, -1.0, 7.25, 2.5};
  int aa[string] = '{"x": 4, "a": 9, "m": 4};
  int f[5] = '{4, 2, 9, 2, 1};
  initial begin
    $display("%p", q.find with (item > 1));
    $display("%p", q.find_last_index with (item < 3));
    $display("%p", q.unique());
    $display("%0d %0d", q.find with (item > 1).size(), q.unique().sum());
    $display("%p", s.unique());
    $display("%p %p", s.min(), s.max());
    $display("%p", s.max with (item.len()));
    $display("%p", r.min());
    $display("%p", r.unique_index());
    $display("%p", q.max with (item * -0.5));
    $display("%p", q.find_index with (item == item.index));
    $display("%p", q.find with (item.index > 2));
    $display("%p", aa.find_index with (item == 4));
    $display("%p", aa.find with (item > 4));
    $display("%p %p", aa.min(), aa.unique());
    $display("%p", aa.find_first_index with (item.index > "b"));
    $display("%0d", aa.sum with (item * 2));
    f.sort();
    $display("%p", f);
    f.rsort();
    $display("%p", f);
    f.reverse();
    $display("%p", f);
    f.sort with (item % 3);
    $display("%p", f);
    f.shuffle();
    $display("%0d", f.sum());
  end
endmodule
//> '{3, 2, 3, 5}
//> '{2}
//> '{3, 1, 2, 5}
//> 4 11
//> '{"pear", "apple", "fig"}
//> '{"apple"} '{"pear"}
//> '{"apple"}
//> '{-1.00000}
//> '{0, 1, 2}
//> '{1}
//> '{1, 2, 3}
//> '{3, 5}
//> '{"m", "x"}
//> '{9}
//> '{4} '{9, 4}
//> '{"m"}
//> 34
//> '{1, 2, 2, 4, 9}
//> '{9, 4, 2, 2, 1}
//> '{1, 2, 2, 4, 9}
//> '{9, 1, 4, 2, 2}
//> 18
