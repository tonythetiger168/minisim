// IEEE 1800-2023 11.6.1 / 11.8.2: the right-hand side is evaluated at the
// largest width of the left side and its operands, then cut to the target, so
// a carry brought down by >> or / is kept.
module t;
  logic [15:0] a, b, r;
  initial begin
    a = 16'hc000;
    b = 16'h6000;
    r = (a + b + 0) >> 1;
    $display("%h", r);
    r = (a + b + 17'd0) / 2;
    $display("%h", r);
    r = (a + b) >> 1;
    $display("%h", r);
  end
endmodule
//> 9000
//> 9000
//> 1000
