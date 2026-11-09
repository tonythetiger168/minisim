// IEEE 1800-2023 7.6: Whole fixed-array assignment, equality, and slices
// (7.6, 7.4.5, 11.4.5).
//! xfail: whole-array assignment and compare are errors
//! icarus-skip: Icarus 13 does not support array compare
module top;
  int a[4], b[4], c[2];
  initial begin
    foreach (a[i]) a[i] = i + 1;
    b = a;
    $display("%0d %0d", b[3], a == b);
    b[0] = 9;
    $display("%0d %0d", a == b, a != b);
    c = a[1:2];
    $display("%0d %0d", c[0], c[1]);
  end
endmodule

//> 4 1
//> 0 1
//> 2 3
