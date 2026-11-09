// IEEE 1800-2023 7.12: array methods on a fixed-size unpacked array: locator
// methods (7.12.1) and reductions (7.12.3), with and without a with clause.
//! icarus-skip: Icarus 13 does not support array locator methods
module t;
  int a[4] = '{4, 2, 8, 2};
  int r[$];
  initial begin
    r = a.find_index with (item == 2); $display("%0d %0d %0d", r.size(), r[0], r[1]);
    r = a.max(); $display("%0d", r[0]);
    r = a.unique(); $display("%0d", r.size());
    $display("%0d %0d", a.sum(), a.sum() with (item > 3 ? item : 0));
  end
endmodule
//> 2 1 3
//> 8
//> 3
//> 16 12
