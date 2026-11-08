// IEEE 1800-2023 9.4.5: a = #d b takes b now and writes it d later, blocking;
// a <= #d b schedules the write d later and goes on. 9.4.1: a delay can be an expression.
module top;
  integer d = 3;
  reg [3:0] a = 0, b = 0, c = 0;
  initial begin
    #d $display("%0d A", $time);
    #(d*2) $display("%0d B", $time);
    c = 4'd5;
    a = #2 c;
    $display("%0d a=%0d", $time, a);
    b <= #3 4'd7;
    $display("%0d b=%0d", $time, b);
    #4 $display("%0d b=%0d", $time, b);
  end
endmodule

//> 3 A
//> 9 B
//> 11 a=5
//> 11 b=0
//> 15 b=7
