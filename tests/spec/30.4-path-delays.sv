// IEEE 1800-2023 30.4: A specify path delay delays the output of a module
// (30.4, 30.5).
//! xfail: specify path delays are ignored
//! icarus-skip: Icarus 13 ignores specify delays unless run with -gspecify
module buf2(input a, output b);
  assign b = a;
  specify
    (a => b) = 2;
  endspecify
endmodule
module top;
  logic a = 0;
  wire b;
  buf2 u(a, b);
  initial begin
    #5 a = 1;
    #1 $display("%b", b);
    #2 $display("%b", b);
  end
endmodule

//> 0
//> 1
