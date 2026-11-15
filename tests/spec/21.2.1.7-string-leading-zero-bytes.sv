// 21.2.1.7, 21.2.1.3: %s prints a value as 8-bit characters. A leading zero
// byte is not printed as a character; like %d, the field keeps one column
// for it and pads with spaces. %0s drops them, and a width pads as given.
module top;
  reg [31:0] r;
  initial begin
    r = "A";
    $display("[%s][%0s][%6s][%-6s]", r, r, r, r);
    r = "hi";
    $display("[%s][%0s]", r, r);
    r = "abcd";
    $display("[%s][%0s]", r, r);
  end
endmodule
//> [   A][A][     A][A     ]
//> [  hi][hi]
//> [abcd][abcd]
