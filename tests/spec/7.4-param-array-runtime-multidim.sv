// 7.4, 7.4.6, 6.20.2: parameter unpacked arrays read with run-time indexes
// beyond a one-dimensional ROM: several dimensions, a part-select of the
// element, real and string elements, and a read inside a function. An index
// outside the array reads the element type's default (x for 4-state, 0 for
// 2-state).
//! icarus-skip: Icarus 13 does not support unpacked array parameters
module top;
  localparam logic [15:0] T[3][0:1] = '{'{16'h0102, 16'h0304}, '{16'h0506, 16'h0708}, '{16'h090a, 16'h0b0c}};
  localparam real R[2] = '{1.5, 2.25};
  localparam string NAMES[3:0] = '{"", "BLUE", "GREEN", "RED"};
  localparam int D[4:1] = '{40, 30, 20, 10};
  parameter int Q[2][2] = '{'{1, 2}, '{3, 4}};
  int i, j, k;
  wire [7:0] w = T[i][j][15:8];
  function automatic int f(int a, int b); return Q[a][b] * 10; endfunction
  initial begin
    for (i = 0; i < 3; i++)
      for (j = 0; j < 2; j++) begin
        #1 $display("%0d %0d %h %h %h", i, j, T[i][j], T[i][j][7:0], w);
      end
    for (k = 0; k < 2; k++) $display("%0.2f", R[k]);
    for (k = 0; k < 4; k++) $display("[%s]", NAMES[k]);
    $display("%0d %0d", f(1, 0), f(0, 1));
    k = 5;
    $display("%h %0d", T[k][0], D[k]);
  end
endmodule
//> 0 0 0102 02 01
//> 0 1 0304 04 03
//> 1 0 0506 06 05
//> 1 1 0708 08 07
//> 2 0 090a 0a 09
//> 2 1 0b0c 0c 0b
//> 1.50
//> 2.25
//> [RED]
//> [GREEN]
//> [BLUE]
//> []
//> 30 20
//> xxxx 0
