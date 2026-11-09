// IEEE 1800-2023 28.16: Gate delays, with separate rise and fall values
// (28.16).
//! xfail: gate delays are errors
module top;
  logic a = 0;
  wire o1, o2;
  not #3 n1(o1, a);
  buf #(2, 5) b1(o2, a);
  initial begin
    #1 $display("%0t %b %b", $time, o1, o2);
    #3 $display("%0t %b %b", $time, o1, o2);
    #2 $display("%0t %b %b", $time, o1, o2);
    a = 1;
    #1 $display("%0t %b %b", $time, o1, o2);
    #3 $display("%0t %b %b", $time, o1, o2);
    a = 0;
    #4 $display("%0t %b %b", $time, o1, o2);
    #2 $display("%0t %b %b", $time, o1, o2);
  end
endmodule

//> 1 x x
//> 4 1 x
//> 6 1 0
//> 7 1 0
//> 10 0 1
//> 14 1 1
//> 16 1 0
