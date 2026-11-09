// IEEE 1800-2023 35.5.6.1: An open array formal (`[]`) takes an unpacked
// array of any size, fixed or dynamic; C reads its shape with svLeft, svRight,
// svLow, svHigh, svIncrement, svSize and svDimensions and its elements with
// svGetArrElemPtr and the element copy functions (H.12). A sized array formal
// is a plain C array (35.5.6).
//! args: data/dpi/arrays.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  import "DPI-C" function int sum_open(input int a[]);
  import "DPI-C" function void shape(input int a[]);
  import "DPI-C" function void squares(output int a[]);
  import "DPI-C" function void fill2(output int m[][]);
  import "DPI-C" function void rev12(inout bit [11:0] v[]);
  import "DPI-C" function void lflip(inout logic l[]);
  import "DPI-C" function int dot4(input int a[4], input int b[4]);
  import "DPI-C" function void iota4(output int a[4]);
  import "DPI-C" function int total_len(input string s[]);

  int up[0:3] = '{1, 2, 3, 4};
  int down[7:5] = '{10, 20, 30};
  int d[];
  int sq[2:5];
  int m[1:2][3:4];
  bit [11:0] v[2];
  logic l[4];
  int a4[4], b4[4];
  string names[3];
  initial begin
    $display("%0d %0d", sum_open(up), sum_open(down));
    shape(up);
    shape(down);
    d = new[5];
    foreach (d[i]) d[i] = i + 1;
    $display("%0d", sum_open(d));
    shape(d);
    squares(sq);
    $display("%0d %0d %0d %0d", sq[2], sq[3], sq[4], sq[5]);
    squares(d);
    $display("%0d %0d %0d", d[0], d[2], d[4]);
    fill2(m);
    $display("%0d %0d %0d %0d", m[1][3], m[1][4], m[2][3], m[2][4]);
    v[0] = 12'h001; v[1] = 12'hc0f;
    rev12(v);
    $display("%h %h", v[0], v[1]);
    l[0] = 0; l[1] = 1; l[2] = 1'bx; l[3] = 1'bz;
    lflip(l);
    $display("%b%b%b%b", l[0], l[1], l[2], l[3]);
    a4 = '{1, 2, 3, 4};
    b4 = '{5, 6, 7, 8};
    $display("%0d", dot4(a4, b4));
    iota4(a4);
    $display("%0d %0d", a4[0], a4[3]);
    names = '{"ab", "cde", ""};
    $display("%0d", total_len(names));
  end
endmodule

//> 10 60
//> dims=1 [0:3] low=0 high=3 inc=-1 size=4 bytes=16
//> dims=1 [7:5] low=5 high=7 inc=1 size=3 bytes=12
//> 15
//> dims=1 [0:4] low=0 high=4 inc=-1 size=5 bytes=20
//> 4 9 16 25
//> 0 4 16
//> 13 14 23 24
//> 800 f03
//> 10xz
//> 70
//> 100 103
//> 5
