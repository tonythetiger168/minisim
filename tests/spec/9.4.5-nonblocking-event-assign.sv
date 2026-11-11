// IEEE 1800-2023 9.4.5: a nonblocking assignment with an intra-assignment
// event control, `a <= @(e) b` and `a <= repeat (n) @(e) b`. The right-hand
// side is evaluated at once, the process goes on, and the update is
// scheduled when the event (the n-th event) occurs. Each execution keeps
// its own value.
module top;
  logic [3:0] a, b = 1, c;
  event e;
  logic clk = 0;
  always #5 clk = ~clk;
  initial begin
    a <= @(e) b;
    b = 2;
    $display("%0t goes on, a=%h", $time, a);
    #3 ->e;
    #1 $display("%0t a=%h", $time, a);
    c <= repeat (2) @(posedge clk) b + 4'd1;
    b = 7;
    #1 $display("%0t c=%h", $time, c);
    #10 $display("%0t c=%h", $time, c);
    #2 $display("%0t c=%h", $time, c);
    for (int i = 0; i < 3; i++) begin
      a <= @(posedge clk) i[3:0];
      #10 $display("%0t a=%h", $time, a);
    end
    $finish;
  end
endmodule
//> 0 goes on, a=x
//> 4 a=1
//> 5 c=x
//> 15 c=x
//> 17 c=3
//> 27 a=0
//> 37 a=1
//> 47 a=2
