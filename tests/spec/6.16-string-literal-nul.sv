// IEEE 1800-2023 6.16: a "\0" in a string literal assigned to a string
// variable is left out of the string.
module t;
  string s;
  initial begin
    s = "a\0b";
    $display("%0d %s %0d", s.len(), s, s == "ab");
  end
endmodule
//> 2 ab 1
