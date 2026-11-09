// IEEE 1800-2023 13.5.3: Default argument values and named argument binding
// (13.5.3, 13.5.4).
module top;
  function int f(int a, int b = 10);
    return a - b;
  endfunction
  initial $display("%0d %0d %0d", f(1), f(.b(1), .a(5)), f(3, 2));
endmodule

//> -9 4 1
