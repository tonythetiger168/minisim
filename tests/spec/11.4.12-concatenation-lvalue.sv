// IEEE 1800-2023 11.4.12 and 10.4.1: a concatenation as the target of blocking,
// nonblocking, delayed nonblocking and continuous assignments. The right-hand
// side is evaluated once and each part takes its slice, first part on top.
module t;
  logic [3:0] a, b; logic c; logic [7:0] q [2];
  wire [3:0] s; wire co;
  assign {co, s} = a + b;
  initial begin
    {a, b} = 8'hA7;
    $display("%h %h", a, b);
    {c, a} = 5'h1F;
    $display("%b %h", c, a);
    {a, {b, c}} = 9'b1100_0011_1;
    $display("%h %h %b", a, b, c);
    a = 4'd9; b = 4'd8;
    #1 $display("%b %h", co, s);
    {a, b} <= 8'h12;
    $display("%h %h", a, b);
    #1 $display("%h %h", a, b);
    {q[0], q[1]} = 16'hBEEF;
    $display("%h %h", q[0], q[1]);
    {a, b} <= #5 8'h34;
    #4 $display("%h %h", a, b);
    #2 $display("%h %h", a, b);
  end
endmodule
//> a 7
//> 1 f
//> c 3 1
//> 1 1
//> 9 8
//> 1 2
//> be ef
//> 1 2
//> 3 4
