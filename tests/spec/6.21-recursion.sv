// IEEE 1800-2023 6.21: An automatic function can call itself; each call has
// its own variables (6.21, 13.4.2).
//! xfail: recursion is an error
module top;
  function automatic int fact(int n);
    if (n <= 1) return 1;
    return n * fact(n - 1);
  endfunction
  function automatic int fib(int n);
    int a, b;
    if (n < 2) return n;
    a = fib(n - 1);
    b = fib(n - 2);
    return a + b;
  endfunction
  initial $display("%0d %0d", fact(5), fib(10));
endmodule

//> 120 55
