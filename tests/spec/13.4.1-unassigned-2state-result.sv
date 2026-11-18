// IEEE 1800-2023 13.4.1 and 6.8: a function's result variable is a variable
// of the return type; a 2-state one starts at 0, so a call that never
// assigns it returns 0, not x. A 4-state result starts at x.
module t;
  int a, r;
  function int never(int x); endfunction
  function int maybe(int x); if (x > 5) maybe = 3; endfunction
  function automatic bit [3:0] nib(int x); endfunction
  function integer four(int x); endfunction
  wire [31:0] w = never(a);
  initial begin
    #1;
    r = never(a);
    $display("%0d %0d %0d %0d %b", w, r, maybe(1), maybe(9), nib(0));
    $display("%0d", four(0));
  end
endmodule
//> 0 0 0 3 0000
//> x
