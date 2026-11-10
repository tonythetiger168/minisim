// IEEE 1800-2023 7.7: Arrays as subroutine arguments. Fixed-size arrays,
// dynamic arrays and queues are passed by value (the callee works on a
// copy); output and inout arguments copy back on return. The same holds
// for class methods and recursive (automatic) subroutines.
//! icarus-skip: Icarus 13 does not pass arrays to functions
module top;
  function automatic int total(int a[4]);
    int s = 0;
    foreach (a[i]) s += a[i];
    a[0] = 100;
    return s;
  endfunction
  function automatic int dsum(int d[]);
    return d.sum();
  endfunction
  function automatic void fill(output byte o[2], output string s[$]);
    o[0] = -1; o[1] = 3;
    s.push_back("hi");
  endfunction
  function automatic int rec(int a[2], int n);
    if (n == 0) return a[0] + a[1];
    a[0]++;
    return rec(a, n - 1);
  endfunction
  class K;
    function int qs(int q[$]); q.push_back(9); return q.sum(); endfunction
    function void dbl(inout int q[$]); foreach (q[i]) q[i] *= 2; endfunction
    task get(output int o[3], output int d[]); o = '{7, 8, 9}; d = new[2]; d[1] = 5; endtask
  endclass
  int x[4] = '{1, 2, 3, 4};
  int y[] = '{5, 6};
  int q[$] = {1, 2};
  byte b[2];
  string ss[$];
  int o[3];
  int d[];
  K k;
  initial begin
    $display("%0d %0d %0d", total(x), x[0], dsum(y));
    fill(b, ss);
    $display("%0d %0d %s %0d", b[0], b[1], ss[0], rec('{1, 2}, 3));
    k = new;
    $display("%0d %0d", k.qs(q), q.size());
    k.dbl(q);
    k.get(o, d);
    $display("%0d %0d %0d %0d %0d %0d", q[0], q[1], o[0], o[2], d.size(), d[1]);
  end
endmodule

//> 10 1 11
//> -1 3 hi 6
//> 12 2
//> 2 4 7 9 2 5
