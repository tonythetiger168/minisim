// IEEE 1800-2023 24.6: an anonymous program in a package declares tasks
// and functions in the package's name space, for programs to call.
package pkg;
  program;
    function automatic int twice(int x);
      return 2 * x;
    endfunction
  endprogram
endpackage

program tb;
  import pkg::*;
  initial $display("twice(21) = %0d", twice(21));
endprogram

//> twice(21) = 42
