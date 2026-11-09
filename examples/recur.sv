// A recursive static function is rejected: its one copy of the arguments
// cannot hold every call. Declared automatic, it recurses (tests/spec/6.21-recursion.sv).
module recur;
  function logic [7:0] fact(input logic [7:0] n);
    if (n == 0) return 8'h01;
    return n * fact(n - 8'h01);
  endfunction
  initial $display("%b", fact(8'h03));
endmodule
