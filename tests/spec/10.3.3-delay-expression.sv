// IEEE 1800-2023 10.3.3: the delay of a continuous assignment or a net
// declaration assignment may be an expression; it is read each time the
// right-hand side changes.
`timescale 1ns/1ns
module t;
  reg [7:0] d;
  reg x;
  wire y1;
  assign #(d) y1 = x;
  wire #(d + 1) y2 = x;
  initial begin
    d = 3;
    x = 0;
    #10 x = 1;
    #2 $display("%0t %b%b", $time, y1, y2);
    #1 $display("%0t %b%b", $time, y1, y2);
    #1 $display("%0t %b%b", $time, y1, y2);
    d = 1;
    #10 x = 0;
    #1 $display("%0t %b%b", $time, y1, y2);
    #1 $display("%0t %b%b", $time, y1, y2);
  end
endmodule
//> 12 00
//> 13 10
//> 14 11
//> 25 01
//> 26 00
