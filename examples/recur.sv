// A recursive static function is legal (13.4.2), but its one copy of the
// argument is shared by every call, so msim warns: the result depends on
// when each operand is read. Declared automatic, each call has its own
// copy (tests/spec/6.21-recursion.sv).
module recur;
  function logic [7:0] fact(input logic [7:0] n);
    if (n == 0) return 8'h01;
    return n * fact(n - 8'h01);
  endfunction
  initial $display("%b", fact(8'h03));
endmodule
