// 21.2.1.4: %v gives the strength of a scalar net. For a vector net each
// bit is given, the most significant first, joined by '_'.
module top;
  wire [3:0] n;
  wire s;
  assign (pull1, strong0) n = 4'b0110;
  assign (weak0, weak1) s = 1'b1;
  initial #1 $display("%v %v", n, s);
endmodule
//> St0_Pu1_Pu1_St0 We1
