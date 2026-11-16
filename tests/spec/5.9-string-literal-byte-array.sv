// 5.9: a string literal can be assigned to an unpacked array of bytes, one
// character per element, left justified: the first character goes into
// the leftmost element and elements past the end of the string are 0.
// Both as an initializer and in a procedural assignment.
//! icarus-skip: Icarus 13 does not convert a string literal to an unpacked byte array
module top;
  byte bh[3:0] = "hi2";
  byte bl[0:3] = "lo2";
  byte unsigned c3[0:4];
  byte cut[0:1];
  initial begin
    c3 = "ab";
    cut = "xyz";
    $display("%h %h %h %h", bh[3], bh[2], bh[1], bh[0]);
    $display("%h %h %h %h", bl[0], bl[1], bl[2], bl[3]);
    $display("%h %h %h %h %h", c3[0], c3[1], c3[2], c3[3], c3[4]);
    $display("%s%s", cut[0], cut[1]);
  end
endmodule
//> 68 69 32 00
//> 6c 6f 32 00
//> 61 62 00 00 00
//> xy
