// IEEE 1800-2023 21.2.1.2: a field width wider than the value pads hex and
// binary values with leading zeros; a width of 0 prints the minimum.
module t;
  logic [7:0] b;
  initial begin
    b = 8'h5;
    $display(":%03h:%08b:%0h:%2h:", b, 3'b101, b, b);
  end
endmodule
//> :005:00000101:5:05:
