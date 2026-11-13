// IEEE 1800-2023 7.12: array methods on a fixed-size array that is not a
// plain variable: a row of a two-dimensional array and an array member of an
// unpacked struct. Ordering methods (7.12.2) write the reordered elements back;
// locator and reduction methods (7.12.1, 7.12.3) read them.
//! icarus-skip: Icarus 13 does not support these array methods
module top;
  int m[2][3] = '{'{5, 2, 3}, '{4, 1, 6}};
  typedef struct { int a[4]; byte b[3]; } s_t;
  s_t s;
  int q[$];
  initial begin
    foreach (m[i]) begin
      q = m[i].find_index with (item > 2);
      $display("%p %0d", q, m[i].sum());
    end
    m[1].rsort();
    m[0].reverse();
    $display("%p", m);
    s.a = '{4, 3, 2, 1};
    s.b = '{8'sd5, -8'sd7, 8'sd0};
    s.a.sort();
    s.b.sort();
    $display("%p %p %0d", s.a, s.b, s.a.max()[0]);
  end
endmodule

//> '{0, 2} 10
//> '{0, 2} 11
//> '{'{3, 2, 5}, '{6, 4, 1}}
//> '{1, 2, 3, 4} '{-7, 0, 5} 4
