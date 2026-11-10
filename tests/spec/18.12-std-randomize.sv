// IEEE 1800-2023 18.12: std::randomize sets variables of the calling scope
// to random values; with {...} adds constraints on them (18.12.1). It
// returns 0 and leaves them alone when the constraints cannot be met.
//! icarus-skip: Icarus 13 does not support std::randomize
module top;
  int a, b;
  int ok = 1;
  initial begin
    repeat (20) begin
      if (!std::randomize(a, b) with { a > 0; a < 10; b == a * 2; }) ok = 0;
      if (!(a > 0 && a < 10 && b == a * 2)) ok = 0;
    end
    $display("%0d", ok);
    a = 7;
    ok = std::randomize(a) with { a > 10; a < 5; };
    $display("%0d %0d", ok, a);
  end
endmodule

//> 1
//> 0 7
