// IEEE 1800-2023 7.12.4: A with clause on reduction and ordering methods
// (7.12.2, 7.12.3, 7.12.4).
//! xfail: with clauses are errors
//! icarus-skip: Icarus 13 does not support with clauses
module top;
  int q[$] = '{3, 1, 2};
  initial begin
    $display("%0d", q.sum() with (item * 2));
    $display("%0d", q.sum(x) with (x > 1));
    q.sort(x) with (-x);
    $display("%0d %0d %0d", q[0], q[1], q[2]);
  end
endmodule

//> 12
//> 2
//> 3 2 1
