// IEEE 1800-2023 7.12.1: Array locator methods with a with clause (7.12.1).
//! xfail: locator methods are errors
//! icarus-skip: Icarus 13 does not support locator methods
module top;
  int q[$] = '{1, 5, 3, 5};
  int r[$];
  initial begin
    r = q.find(x) with (x > 2);
    $display("%0d %0d", r.size(), r[0]);
    r = q.find_index with (item == 5);
    $display("%0d %0d", r[0], r[1]);
    r = q.find_first with (item > 1);
    $display("%0d", r[0]);
    r = q.find_last_index with (item == 5);
    $display("%0d", r[0]);
    r = q.min(); $display("%0d", r[0]);
    r = q.max(); $display("%0d", r[0]);
    r = q.unique(); $display("%0d", r.size());
  end
endmodule

//> 3 5
//> 1 3
//> 5
//> 3
//> 1
//> 5
//> 3
