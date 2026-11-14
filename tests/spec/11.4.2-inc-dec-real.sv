// IEEE 1800-2023 11.4.2: ++ and -- on a real or shortreal add or subtract 1.0.
module t;
  real r, q;
  shortreal s;
  int n;
  initial begin
    r = 2.25;
    r++;
    $display("%.2f", r);
    q = r--;
    $display("%.2f %.2f", q, r);
    q = --r;
    $display("%.2f %.2f", q, r);
    s = 0.5;
    ++s;
    s++;
    $display("%.2f", s);
    n = 0;
    for (real x = 0.5; x < 3.0; x++) n++;
    $display("%0d", n);
  end
endmodule
//> 3.25
//> 3.25 2.25
//> 1.25 1.25
//> 2.50
//> 3
