// IEEE 1800-2023 5.7.1: '0 '1 'x 'z fill every bit of the target (5.7.1).
//! xfail: '0 / '1 literals are an error
module top;
  logic [7:0] a;
  logic [69:0] w;
  initial begin
    a = '1; $display("%h", a);
    a = '0; $display("%h", a);
    a = 'x; $display("%h", a);
    a = 'z; $display("%h", a);
    w = '1; $display("%0d", $countones(w));
    $display("%h", a | '1);
  end
endmodule

//> ff
//> 00
//> xx
//> zz
//> 70
//> ff
