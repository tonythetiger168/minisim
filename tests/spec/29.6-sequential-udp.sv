// IEEE 1800-2023 29.6: An edge-sensitive sequential UDP: a D flip-flop with
// an initial value (29.6, 29.7).
//! icarus-skip: Icarus 13 does not parse UDPs inside this file layout
primitive dff(output reg q, input c, d);
  initial q = 0;
  table
    r 0 : ? : 0;
    r 1 : ? : 1;
    f ? : ? : -;
    ? * : ? : -;
  endtable
endprimitive
module top;
  logic c = 0, d = 1;
  wire q;
  dff u(q, c, d);
  initial begin
    #1 $display("%b", q);
    c = 1; #1 $display("%b", q);
    d = 0; #1 $display("%b", q);
    c = 0; #1 c = 1; #1 $display("%b", q);
  end
endmodule

//> 0
//> 1
//> 1
//> 0
