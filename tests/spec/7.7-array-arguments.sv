// IEEE 1800-2023 7.7: Fixed arrays and dynamic arrays as subroutine
// arguments (7.7).
//! xfail: arrays as arguments are an error
//! icarus-skip: Icarus 13 does not pass arrays to functions
module top;
  function automatic int total(int a[4]);
    int s = 0;
    foreach (a[i]) s += a[i];
    return s;
  endfunction
  function automatic int dsum(int d[]);
    return d.sum();
  endfunction
  int x[4] = '{1, 2, 3, 4};
  int y[] = '{5, 6};
  initial $display("%0d %0d", total(x), dsum(y));
endmodule

//> 10 11
