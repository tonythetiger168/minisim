// IEEE 1800-2023 28.8: A bidirectional pass switch connects two nets
// (28.8).
module top;
  logic d = 1, en = 1;
  wire a, b, c;
  assign a = d;
  tran t(a, b);
  tranif1 t1(b, c, en);
  initial begin
    #1 $display("%b %b", b, c);
    en = 0; #1 $display("%b %b", b, c);
  end
endmodule

//> 1 1
//> 1 z
