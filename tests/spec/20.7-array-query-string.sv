//! icarus-skip: Icarus has no $low / $high of a string
// IEEE 1800-2023 20.7: a string has one dimension for the array query
// functions; its characters are indexed from 0 to len-1, as foreach (18.5.7.1)
// treats a string like a dynamic array of bytes.
module t;
  string s = "abcd", e;
  initial begin
    $display("%0d %0d %0d", $size(s), $left(s), $right(s));
    $display("%0d %0d %0d %0d", $low(s), $high(s), $increment(s), $dimensions(s));
    $display("%0d", $size(e));
    for (int i = 0; i < $size(s); i++) if (s[i] == "b") s[i] = "x";
    $display("%s", s);
  end
endmodule
//> 4 0 3
//> 0 3 -1 1
//> 0
//> axcd
