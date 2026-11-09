// IEEE 1800-2023 28.7: nmos, pmos, rnmos switches pass or block by gate
// (28.7).
module top;
  logic d = 1, g = 1;
  wire o1, o2;
  nmos n(o1, d, g);
  pmos p(o2, d, g);
  initial begin
    #1 $display("%b %b", o1, o2);
    g = 0; #1 $display("%b %b", o1, o2);
  end
endmodule

//> 1 z
//> z 1
