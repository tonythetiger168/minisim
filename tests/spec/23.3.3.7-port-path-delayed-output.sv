// 23.3.3.7, 30.4: a port passes on the value of the net inside it. When
// that net is a module path output, its value changes only after the path
// delay, at strong strength, even though the gate behind it already drives
// the new value (or z).
//! icarus-skip: the harness runs Icarus without -gspecify
`timescale 1ns/1ns
module tbuf(input a, en, output y);
  bufif1(y, a, en);
  specify
    (a => y) = (1, 2);
    (en => y) = (3, 4);
  endspecify
endmodule
module top;
  reg a, en;
  wire y;
  tbuf u(a, en, y);
  initial begin
    $monitor("%0t y=%b", $time, y);
    a = 0; en = 1;
    #10 en = 0;
    #10 en = 1;
    #10 a = 1;
    #10 en = 0;
  end
endmodule
//> 0 y=x
//> 2 y=0
//> 13 y=z
//> 24 y=0
//> 31 y=1
//> 44 y=z
