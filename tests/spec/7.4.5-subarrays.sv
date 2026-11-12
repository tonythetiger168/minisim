// IEEE 1800-2023 7.4.5, 7.6: with fewer indices than dimensions, m[i] of a
// multidimensional unpacked array is itself an array. It can be read, written
// (also with a variable index, and nonblocking), compared, passed to a
// subroutine and printed with %p (21.2.1.6).
//! icarus-skip: Icarus 13 does not assign or pass array slices
//> '{10, 11, 12, 13}
//> '{20, 21, 22, 23}
//> '{'{20, 21, 22, 23}, '{10, 11, 12, 13}, '{7, 7, 7, 7}}
//> 0 1
//> 46
//> '{'{'{0, 0}, '{3, 4}}, '{'{1, 2}, '{3, 4}}}
//> '{'{"", ""}, '{"a", "b"}}
//> '{0.500000, 1.50000}
//> '{7, 7, 7, 7} '{10, 11, 12, 13}
module top;
  int m[3][4];
  byte c[2][2][2];
  string sm[2][2];
  real rm[2][2];
  int row[4];
  int i = 2;
  function automatic int rsum(int r[4]); int s = 0; foreach (r[k]) s += r[k]; return s; endfunction
  initial begin
    foreach (m[a, b]) m[a][b] = a * 10 + b;
    row = m[1];
    $display("%p", row);
    m[0] = m[i];
    $display("%p", m[0]);
    m[i] = '{7, 7, 7, 7};
    $display("%p", m);
    $display("%0d %0d", m[0] == m[1], m[1] != m[0]);
    $display("%0d", rsum(m[1]));
    c[1] = '{'{1, 2}, '{3, 4}};
    c[0][1] = c[1][1];
    $display("%p", c);
    sm[1] = '{"a", "b"};
    $display("%p", sm);
    rm[0] = '{0.5, 1.5};
    $display("%p", rm[0]);
    m[1] <= m[2];
    m[2] <= m[1];
    #1 $display("%p %p", m[1], m[2]);
  end
endmodule
