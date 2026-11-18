// IEEE 1800-2023 13.5.3: a default argument value is evaluated each time a
// call uses it. A default may name an earlier argument of the same call (it
// reads the value passed for it) or, in a method, a property of the object
// the method is called on.
// 11.3.5, 11.4.7: the right operand of -> is not evaluated when the left one
// is false, so a function call there (with an output argument) does not run.
//! icarus-skip: Icarus 13 does not support classes
class Counter;
  int base;
  function new(int b); base = b; endfunction
  function int level(int sev, int verb = (sev > 2) ? 10 : 20, int total = sev + verb);
    return total;
  endfunction
  function int offset(int n, int by = n * base);
    return by;
  endfunction
endclass

module top;
  function automatic int twice(int x, int y = x * 2);
    return y;
  endfunction
  function automatic bit put(input int s, output int v);
    v = s;
    return s > 0;
  endfunction
  int a;
  bit x, r;
  initial begin
    Counter c3, c5;
    c3 = new(3);
    c5 = new(5);
    $display("%0d %0d %0d", c3.level(1), c3.level(4), c3.level(4, 1));
    $display("%0d %0d %0d", c3.offset(2), c5.offset(2), c5.offset(2, 9));
    $display("%0d %0d", twice(21), twice(21, 1));
    x = 0; a = 11;
    r = x -> put(7, a);
    $display("%b %0d", r, a);
    x = 1; a = 11;
    r = x -> put(-7, a);
    $display("%b %0d", r, a);
    a = 11;
    if (!x -> put(9, a)) a = a + 1000;
    $display("%0d", a);
  end
endmodule
//> 21 14 5
//> 6 10 9
//> 42 1
//> 1 11
//> 0 -7
//> 1011
