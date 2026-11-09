// IEEE 1800-2023 10.6.2: force and release on a variable and on a net
// (10.6.2).
//! xfail: force/release are errors
module top;
  logic v = 0, a = 0;
  wire w;
  assign w = a;
  initial begin
    force v = 1; force w = 1;
    #1 $display("%b %b", v, w);
    v = 0; a = 0;
    #1 $display("%b %b", v, w);
    release v; release w;
    #1 $display("%b %b", v, w);
    v = 0; #1 $display("%b", v);
  end
endmodule

//> 1 1
//> 1 1
//> 1 0
//> 0
