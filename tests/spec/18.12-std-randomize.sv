// IEEE 1800-2023 18.12: std::randomize sets variables to values that meet
// the with constraints (18.12).
//! xfail: std::randomize is ignored
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
  end
endmodule

//> 1
