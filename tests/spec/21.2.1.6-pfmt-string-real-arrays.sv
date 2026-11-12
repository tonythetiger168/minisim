// IEEE 1800-2023 21.2.1.6, 7.4: %p of fixed-size arrays of strings and reals
// prints the element values, also for one row of a multidimensional array.
// An array method result can be indexed directly (7.12).
//! icarus-skip: Icarus 13 does not support %p
//> '{"", "", "z"} '{0, 5}
//> '{'{"", "y"}, '{"x", ""}}
//> '{0.00000, 2.50000} '{"q", ""}
//> 4 1
module top;
  string s1[3];
  int a[2];
  string st[2][2];
  real r[2];
  string s[2];
  int v[4] = '{3, 1, 4, 2};
  initial begin
    s1[2] = "z"; a[1] = 5;
    st[1][0] = "x"; st[0][1] = "y";
    r[1] = 2.5; s[0] = "q";
    #1 $display("%p %p", s1, a);
    $display("%p", st);
    $display("%p %p", r, s);
    $display("%0d %0d", v.max()[0], v.min()[0]);
  end
endmodule
