// IEEE 1800-2023 6.12: real arithmetic and comparison; 6.12.1 real to integer
// rounds; 20.5 $itor $rtoi $realtobits $bitstoreal; 20.8 math functions;
// 21.2.1.3 %f %e %g, and a real with no format prints like %g.
module top;
  real r = 1.5, q;
  integer i;
  initial begin
    q = r * 2 + 1;
    i = r;
    $display(r, " ", q);
    $display("%f %e %g %0.2f %8.3f %d %0d", q, q, q, q, q, i, q);
    $display("%g %g", 1.0e-7, 123456789.0);
    i = -2.5; $display("%0d", i);
    $display("%0d %0d", r > 1.0, r == 1.5);
    $display("%f", $itor(3) / 2);
    $display("%0d", $rtoi(2.9));
    $display("%f %f", $sqrt(2.0), $pow(2.0, 10));
    #3 $display("%f", $realtime);
    $display("%h", $realtobits(1.0));
    $display("%f", $bitstoreal(64'h4000000000000000));
    $display("%0d", r);
  end
endmodule

//> 1.50000 4.00000
//> 4.000000 4.000000e+00 4 4.00    4.000           2 4
//> 1e-07 1.23457e+08
//> -3
//> 1 1
//> 1.500000
//> 2
//> 1.414214 1024.000000
//> 3.000000
//> 3ff0000000000000
//> 2.000000
//> 2
