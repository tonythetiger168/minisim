// IEEE 1800-2023 6.12.1: integer to real and back, real in ?: and functions,
// compound assignment; 21.2.1.3 widths, precision and left-justify.
module top;
  real r, a[0:1];
  integer i = -3;
  int k;
  function real half(input real x); return x / 2; endfunction
  initial begin
    $display("%f %0d", r, k);
    r = i; $display("%f", r);
    r = i / 2; $display("%f", r);
    r = i / 2.0; $display("%f", r);
    r += 0.25; $display("%f", r);
    a[1] = half(5); $display("%f", a[1]);
    r = (i < 0) ? 1.5 : 2.5; $display("%f", r);
    k = 7.5; $display("%0d", k);
    k = -7.5; $display("%0d", k);
    if (r) $display("nonzero");
    r = 2 ** 0.5; $display("%.4f", r);
    $display("%e", 1234.5);
    $display("%10.3f|%-10.2f|", 3.14159, 2.5);
  end
endmodule

//> 0.000000 0
//> -3.000000
//> -1.000000
//> -1.500000
//> -1.250000
//> 2.500000
//> 1.500000
//> 8
//> -8
//> nonzero
//> 1.4142
//> 1.234500e+03
//>      3.142|2.50      |
