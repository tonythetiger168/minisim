// IEEE 1800-2023 6.20.3: A type parameter, overridden at instantiation
// (6.20.3).
//! icarus-skip: Icarus 13 orders the three initial blocks differently
module m #(type T = int) ();
  T x;
  initial begin x = -1; $display("%0d %0d", x, $bits(x)); end
endmodule
module top;
  m u1();
  m #(.T(byte)) u2();
  m #(.T(logic [3:0])) u3();
endmodule

//> -1 32
//> -1 8
//> 15 4
