// IEEE 1800-2017 11.4.9 Reduction operators, including ~& ~| ~^.
module t;
  logic [3:0] a = 4'b1011;
  initial $display("%b %b %b %b %b %b", &a, |a, ^a, ~&a, ~|a, ~^a);
endmodule
//> 0 1 1 1 0 0
