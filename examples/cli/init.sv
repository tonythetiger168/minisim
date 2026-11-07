// Declaration initializers (IEEE 1800 6.8 and 6.7.1).
// v is set before any initial block runs, so the first $display sees 5.
// w = v + 1 is a continuous assignment and follows later changes of v.
module init_tb;
  logic [7:0] v = 5;
  wire  [7:0] w = v + 1;
  initial begin
    $display("v=%d", v);
    #1 $display("w=%d", w);
    v = 9;
    #1 $display("w=%d", w);
  end
endmodule
