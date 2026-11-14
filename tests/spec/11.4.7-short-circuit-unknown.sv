// IEEE 1800-2023 11.4.7: the right operand of && or || is skipped only when
// the left operand alone decides the result; an x on the left still runs it.
module t;
  int calls;
  logic u, r;
  function bit f();
    calls++;
    return 1;
  endfunction
  initial begin
    calls = 0;
    u = 1'bx;
    r = 1'b0 && f();
    r = 1'b1 || f();
    $display("%0d", calls);
    r = u && f();
    r = u || f();
    $display("%0d", calls);
  end
endmodule
//> 0
//> 2
