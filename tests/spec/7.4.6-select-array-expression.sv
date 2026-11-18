// 7.4.6: selects of an unpacked array that is not a variable: a slice of an
// array member of an unpacked struct (s.arr[3:1], s.arr[i +: w] with a
// constant start), and an element of a function's array result (f()[i][j]).
//! icarus-skip: Icarus does not support unpacked structs
module top;
  typedef struct { logic [31:0] arr[3:0]; } a_t;
  typedef struct { int arr[0:3]; } b_t;
  typedef int grid_t[2][4];
  typedef logic [7:0] row_t[1:3];
  a_t a, b;
  b_t c, d;
  int i, j = 3;
  function automatic b_t mk(); b_t x; x.arr = '{5, 6, 7, 8}; return x; endfunction
  function grid_t grid(); return '{'{1, 2, 3, 4}, '{5, 6, 7, 8}}; endfunction
  function row_t rows(int base); return '{base, base + 1, base + 2}; endfunction
  initial begin
    a.arr = '{4, 3, 2, 1};
    b.arr[3:1] = a.arr[3:1];
    b.arr[0] = 0;
    c.arr = '{1, 2, 3, 4};
    d.arr[0:1] = c.arr[2:3];
    d.arr[2:3] = mk().arr[1 +: 2];
    $display("%p %p", b, d);
    b.arr[2:0] = a.arr[3 -: 3];
    $display("%p", b);
    i = grid()[1][2];
    $display("%0d %0d", i, grid()[0][j]);
    $display("%0d %0d", rows(10)[3], rows(20)[j - 2]);
  end
endmodule
//> '{arr:'{4, 3, 2, 0}} '{arr:'{3, 4, 6, 7}}
//> '{arr:'{4, 4, 3, 2}}
//> 7 4
//> 12 20
