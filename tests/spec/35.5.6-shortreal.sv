// IEEE 1800-2023 35.5.6: a shortreal DPI argument or result is a C float,
// by value for inputs and results and by pointer for outputs and inouts.
//! args: data/dpi/shortreal.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  import "DPI-C" function shortreal c_scale(input shortreal a, input shortreal k);
  import "DPI-C" function void c_split(input shortreal a, output shortreal whole, output shortreal frac);
  import "DPI-C" function void c_half(inout shortreal x);
  shortreal w, f, h;
  initial begin
    $display("%0.3f", c_scale(1.5, 2.25));
    c_split(7.75, w, f);
    $display("%0.2f %0.2f", w, f);
    h = 9.0;
    c_half(h);
    $display("%0.2f", h);
  end
endmodule

//> 3.375
//> 7.00 0.75
//> 4.50
