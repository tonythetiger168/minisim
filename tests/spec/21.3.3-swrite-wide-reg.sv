// IEEE 1800-2023 21.3.3: $sformat / $swrite into an integral variable store
// the text right-justified, the last character in the low byte, however
// wide the variable is.
module t;
  reg [8*24:1] r;
  initial begin
    $swrite(r, "%s", "abcdefghijklmnop");
    $display("%0s", r);
    $sformat(r, "n=%0d and more text", 42);
    $display("%0s", r);
  end
endmodule
//> abcdefghijklmnop
//> n=42 and more text
