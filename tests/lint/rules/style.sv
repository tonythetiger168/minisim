//! args: -only style -enable STY002,STY003 -top style
module style(input logic clk, a, b, output logic y, z);
  logic k;
  assign k = a;
  function automatic logic f1(input logic x);     //~ FUNC001
    return x & k;                 // k is not an argument
  endfunction                     
  function automatic logic f2(input logic x, input logic w);
    return x & w;
  endfunction
  always @(posedge clk) y <= f1(b);       //~ STY003 STY013
  always @* z = f2(a, b);                 //~ STY003 STY013
endmodule

module style2(input logic a, output logic y);    //~ STY001 STY002
  assign y = a;
endmodule
