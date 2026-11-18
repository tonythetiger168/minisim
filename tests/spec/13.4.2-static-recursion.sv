// IEEE 1800-2023 13.4.2 and 13.3: a static function or task may call
// itself, directly or through another subroutine; every call shares the
// one copy of its arguments and variables.
module t;
  int n = 0, depth;
  logic [3:0] a = 3, y;
  function int down(int k);   // static: k is shared by every call
    n++;
    if (k > 0) void'(down(k - 1));
    return k;
  endfunction
  function logic [3:0] f(logic [3:0] v);
    if (v == 0) return 0;
    return f(v - 1) + 4'd2;
  endfunction
  task static tk(int k);
    #1 depth++;
    if (k > 0) tk(k - 1);
  endtask
  always_comb y = f(a);
  initial begin
    $display("%0d %0d", down(3), n);
    tk(2);
    $display("%0d %0d", depth, y);
    a = 2; #0 $display("%0d", y);
  end
endmodule
//> 0 4
//> 3 6
//> 4
