// IEEE 1800-2023 29.4: A combinational UDP with a table (29.4).
//! xfail: UDPs are errors
//! icarus-skip: Icarus 13 does not parse UDPs inside this file layout
primitive mux2(output y, input s, a, b);
  table
    0 0 ? : 0;
    0 1 ? : 1;
    1 ? 0 : 0;
    1 ? 1 : 1;
    x 0 0 : 0;
    x 1 1 : 1;
  endtable
endprimitive
module top;
  logic s = 0, a = 1, b = 0;
  wire y;
  mux2 m(y, s, a, b);
  initial begin
    #1 $display("%b", y);
    s = 1; #1 $display("%b", y);
    s = 1'bx; #1 $display("%b", y);
  end
endmodule

//> 1
//> 0
//> x
