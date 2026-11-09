// IEEE 1800-2023 35.5.4: Import declarations: a C name other than the SV
// name (c_identifier =), pure and context properties, import tasks, imports
// in a package, and import calls in continuous assignments and always_comb.
//! args: data/dpi/decl.c
//! icarus-skip: Icarus 13 does not support DPI
package util;
  import "DPI-C" pure function int c_sq(input int a);
endpackage

module top;
  import util::*;
  import "DPI-C" c_sum3 = function int sum3(input int a, input int b, input int c);
  import "DPI-C" pure function int c_twice(input int a);
  import "DPI-C" context function int c_count();
  import "DPI-C" task c_task(input int n, output int out);
  import "DPI-C" function void c_hello();

  int a = 3;
  wire [31:0] w = c_twice(a);
  int y;
  always_comb y = c_sq(a);
  int r;
  initial begin
    c_hello();
    $display("%0d", sum3(1, 2, 3));
    #1 $display("%0d %0d", w, y);
    a = 5;
    #1 $display("%0d %0d", w, y);
    c_task(41, r);
    $display("%0d", r);
    void'(c_count());
    $display("%0d", c_count());
  end
endmodule

//> hello from C
//> 6
//> 6 9
//> 10 25
//> 42
//> 2
