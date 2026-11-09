// IEEE 1800-2023 6.12: shortreal is a 32-bit IEEE float (6.12).
//! xfail: shortreal is an error
//! icarus-skip: Icarus 13 gives $bits(shortreal) as 1
module top;
  shortreal r;
  initial begin
    r = 1.5;
    $display("%f", r);
    r = r * 3;
    $display("%f %0d", r, $bits(r));
  end
endmodule

//> 1.500000
//> 4.500000 32
