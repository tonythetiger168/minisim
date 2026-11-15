// 21.2.1.7, 21.2.1.3: %s prints a value as 8-bit characters. A leading zero
// byte is not printed as a character; like %d, the field keeps one column
// for it and pads with spaces. %0s drops them, and a width pads as given.
// A zero byte after the first character prints as a space. A literal
// keeps its zero bytes, and "" is one zero byte (11.10.3).
module top;
  reg [31:0] r;
  reg [95:0] w;
  initial begin
    r = "A";
    $display("[%s][%0s][%6s][%-6s]", r, r, r, r);
    r = "hi";
    $display("[%s][%0s]", r, r);
    r = "abcd";
    $display("[%s][%0s]", r, r);
    r = 32'h00610062;
    $display("[%s][%0s]", r, r);
    r = 32'h61626300;
    $display("[%s][%0s]", r, r);
    w = 0;
    w[39:0] = 40'h6100620063;
    $display("[%s][%0s]", w, w);
    $display("[%s][%0s][%s][%0s]", "\000a\000b", "\000a\000b", "", "");
  end
endmodule
//> [   A][A][     A][A     ]
//> [  hi][hi]
//> [abcd][abcd]
//> [ a b][a b]
//> [abc ][abc ]
//> [       a b c][a b c]
//> [ a b][a b][ ][]
