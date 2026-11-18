//! icarus-skip: Icarus cannot evaluate a parameter of a nested unpacked array type
// IEEE 1800-2023 7.6, 7.4.5: a sub-array of a parameter array, selected
// with constant indexes, copied into an array element of matching shape,
// in a function and in a procedure. Elements go left to right whatever
// the direction of either range.
module top;
  typedef int row_t[0:2];
  typedef int up_t[0:1][0:2];
  typedef int dn_t[1:0][2:0];
  localparam up_t P = '{'{1, 2, 3}, '{4, 5, 6}};
  function automatic dn_t flip();
    flip[1] = P[1];
    flip[0] = P[0];
  endfunction
  dn_t d;
  row_t r;
  initial begin
    d = flip();
    r = P[1];
    $display("%p %p", d, r);
    d[0] = P[0];
    $display("%0d %0d", d[0][2], d[0][0]);
  end
endmodule
//> '{'{4, 5, 6}, '{1, 2, 3}} '{4, 5, 6}
//> 1 3
