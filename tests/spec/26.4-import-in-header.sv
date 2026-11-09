// IEEE 1800-2023 26.4: A package import in a module header (26.4).
package pk; typedef logic [3:0] n_t; endpackage
module top import pk::*; ();
  n_t x;
  initial begin x = 4'h3; $display("%h %0d", x, $bits(x)); end
endmodule

//> 3 4
