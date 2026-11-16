// 7.4.5, 7.4.6, 10.3: slices of unpacked arrays beyond one dimension and
// one variable: a slice of the first dimension of a multidimensional array
// (read and written), a slice of a parameter array, and a continuous
// assignment to a slice or a sub-array of a net array.
//! icarus-skip: Icarus 13 does not support unpacked array parameters
module top;
  parameter int P[4:-3] = '{'h100, 'h101, 'h102, 'h103, 'h104, 'h105, 'h106, 'h107};
  logic [7:0] m[3:0][2:0];
  logic [7:0] n[2:1][2:0];
  int two[1:0];
  wire [7:0] w[0:5];
  wire [3:0] g[1:0][0:1];
  assign w[1:3] = '{8'h11, 8'h22, 8'h33};
  assign w[4+:2] = '{8'h44, 8'h55};
  assign g[1] = '{4'ha, 4'hb};
  initial begin
    foreach (m[i, j]) m[i][j] = 8'(i * 10 + j);
    n[2:1] = m[1:0];
    $display("%0d %0d %0d %0d %0d %0d", n[2][2], n[2][1], n[2][0], n[1][2], n[1][1], n[1][0]);
    m[3:2] = n;
    $display("%0d %0d %0d", m[3][2], m[3][0], m[2][1]);
    two = P[3:2];
    $display("%h %h", two[1], two[0]);
    two = P[-3+:2];
    $display("%h %h", two[1], two[0]);
    #1 $display("%h %h %h %h %h %h", w[0], w[1], w[2], w[3], w[4], w[5]);
    $display("%h %h %b %b", g[1][0], g[1][1], g[0][0], g[0][1]);
  end
endmodule
//> 12 11 10 2 1 0
//> 12 10 1
//> 00000101 00000102
//> 00000106 00000107
//> zz 11 22 33 44 55
//> a b zzzz zzzz
