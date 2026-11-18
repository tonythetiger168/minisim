// IEEE 1800-2023 11.4.7: the logical implication (->) and equivalence (<->)
// operators also take real operands; a real is true when it is not 0.0.
module t;
  real a, b;
  initial begin
    a = 0.5; b = 0.0;
    $display("%0d %0d", a -> b, a <-> b);
    $display("%0d %0d", b -> a, b <-> 0.0);
    $display("%0d %0d", (a * 0.0) -> b, 2.5 <-> a);
  end
endmodule
//> 0 0
//> 1 1
//> 1 1
