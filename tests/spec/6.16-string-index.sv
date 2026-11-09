// IEEE 1800-2023 6.16: s[i] reads byte i of a string (0 when out of
// range); s[i] = c replaces it (no effect when out of range).
module top;
  string s = "CAB";
  int i = 1;
  initial begin
    $display("%0d %0d %0d %0d", s[i], s[1], s[9], s[0] == "C");
    s[1] = "x";
    s[7] = "y";
    $display("%s", s);
  end
endmodule

//> 65 65 0 1
//> CxB
