// IEEE 1800-2023 35.5.6.1, H.7.1, H.12.2: a DPI formal may leave its packed
// dimension open (`bit []`, `logic []`), alone or with unpacked dimensions.
// C gets an svOpenArrayHandle; dimension 0 is the packed part, normalized
// to [n-1:0] with n the width of the actual at that call.
//! args: data/dpi/packedopen.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  import "DPI-C" function int popcount(input bit [] v);
  import "DPI-C" function void evens(output logic [] v);
  import "DPI-C" function longint sum_rows(input bit [] a []);
  bit [7:0] b8 = 8'b1011_0110;
  bit [99:0] b100;
  logic [4:0] l5;
  logic [11:4] l8;
  bit [39:0] rows [3];
  initial begin
    b100[99] = 1; b100[0] = 1; b100[50] = 1;
    $display("%0d %0d %0d", popcount(b8), popcount(b100), popcount(4'b0111));
    evens(l5);
    evens(l8);
    $display("%b %b", l5, l8);
    rows[0] = 40'h10_0000_0000; rows[1] = 5; rows[2] = 7;
    $display("%0d", sum_rows(rows));
  end
endmodule
//> C: [7:0] width 8 ones 5
//> C: [99:0] width 100 ones 3
//> C: [3:0] width 4 ones 3
//> 5 3 3
//> x0101 x1010101
//> C: 3 rows of 40 bits
//> 68719476748
