// IEEE 1800-2023 11.4.3 Arithmetic operators: unary minus and plus, and **
// (table 11-4: negative exponents, X operands).
module t;
  logic [7:0] u;
  logic signed [7:0] s;
  int i;
  initial begin
    u = 8'd3;
    u = -u;
    s = -8'sd5;
    $display("%0d %0d %0d", u, s, +s);
    i = 2 ** 10;
    $display("%0d %0d %0d", i, (-2) ** 3, 2 ** -1);
    $display("%b", 4'b1x00 ** 2);
  end
endmodule
//> 253 -5 -5
//> 1024 -8 0
//> xxxx
