// IEEE 1800-2023 5.9 String literals; 21.2.1.7 %s, %c
// A string literal is 8 bits per character, first character in the top byte.
module t;
  logic [23:0] s;
  initial begin
    s = "abc";
    $display("%s|%h|%c", s, s, s);
    $display("%s", "hi");
  end
endmodule
//> abc|616263|c
//> hi
