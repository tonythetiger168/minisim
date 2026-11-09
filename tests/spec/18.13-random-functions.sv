// IEEE 1800-2023 18.13: $urandom, $urandom_range and $random return values
// in range (18.13).
module top;
  int a, ok = 1;
  int unsigned u;
  initial begin
    repeat (100) begin
      a = $urandom_range(3, 1);
      if (a < 1 || a > 3) ok = 0;
      a = $urandom_range(5);
      if (a < 0 || a > 5) ok = 0;
    end
    u = $urandom;
    a = $random;
    $display("%0d", ok);
  end
endmodule

//> 1
