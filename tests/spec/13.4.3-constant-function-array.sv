// IEEE 1800-2023 13.4.3 Constant functions, 6.20.1 parameter arrays: a
// constant function returning an unpacked array drives a continuous
// assignment, and an element of a parameter array is part-selected.
//! icarus-skip: Icarus 13 does not support unpacked array parameters
module t;
  typedef int unsigned tbl_t [3][2];
  function automatic tbl_t mk(input int unsigned seed);
    tbl_t r;
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 2; j++) r[i][j] = seed * 10 + i * 2 + j;
    return r;
  endfunction
  tbl_t tbl;
  assign tbl = mk(4);
  localparam logic [7:0] Masks [4] = '{8'h11, 8'h2c, 8'hf0, 8'h5a};
  logic [3:0] m;
  assign m = Masks[2][7:4];
  initial #1 $display("%0d %0d %0d %h %h %b", tbl[0][0], tbl[1][1], tbl[2][0], Masks[1], m, Masks[3][1]);
endmodule
//> 40 43 44 2c f 1
