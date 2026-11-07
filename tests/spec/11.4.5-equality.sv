// IEEE 1800-2023 11.4.5 Equality operators: == / != give x with an x or z
// operand; === / !== compare x and z bits exactly.
module t;
  logic [1:0] a;
  initial begin
    a = 2'bx1;
    $display("%b %b %b %b", a == 2'bx1, a === 2'bx1, a !== 2'b01, a != 2'b01);
  end
endmodule
//> x 1 1 x
