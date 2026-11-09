// IEEE 1800-2023 7.10, 7.8, 11.4.5: == and != (and === / !==) on whole
// queues, dynamic arrays and associative arrays: equal sizes and equal
// elements (for associative arrays, equal keys too).
//! icarus-skip: Icarus 13 does not support comparing queues
module top;
  int q1[$], q2[$];
  int d1[], d2[];
  string a1[string], a2[string];
  initial begin
    q1 = '{1, 2, 3}; q2 = '{1, 2, 3};
    $display("%0d %0d", q1 == q2, q1 != q2);
    q2[2] = 4;
    $display("%0d %0d", q1 == q2, q1 !== q2);
    d1 = '{1, 2, 3}; d2 = '{1, 2, 3};
    $display("%0d", d1 == d2);
    a1["x"] = "1"; a2["x"] = "1";
    $display("%0d", a1 == a2);
    a2["y"] = "2";
    $display("%0d", a1 == a2);
  end
endmodule

//> 1 0
//> 0 1
//> 1
//> 1
//> 0
