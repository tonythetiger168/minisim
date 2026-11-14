// IEEE 1800-2023 5.7.1: an unsized unsigned literal whose leftmost bit is x
// or z is extended with x or z to the width of the expression.
module t;
  logic [63:0] v;
  logic [39:0] w;
  initial begin
    v = 'hx5;
    $display("%h", v);
    v = 'hz;
    $display("%h", v);
    w = 'bx;
    $display("%h", w);
    v = 'h1x;
    $display("%h", v);
  end
endmodule
//> xxxxxxxxxxxxxxx5
//> zzzzzzzzzzzzzzzz
//> xxxxxxxxxx
//> 000000000000001x
