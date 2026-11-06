// Flat module: context-determined width, and $display / $strobe / $monitor / $finish.
// wide  = 4'hF + 4'h1 in 8 bits is 00010000 (16), not the 4-bit wrap 0.
// narrow = the same add in 4 bits wraps to 0000.
// b = 0; b <= 1; $display sees 0, $strobe and $monitor see 1.
module checks;
  logic [7:0] wide;
  logic [3:0] narrow;
  logic b;
  initial begin
    wide = 4'hF + 4'h1;
    $display("W %b", wide);
    narrow = 4'hF + 4'h1;
    $display("N %b", narrow);
    b = 0;
    b <= 1;
    $display("D %b", b);
    $strobe("S %b", b);
    $monitor("M %b", b);
    $finish;
  end
endmodule
