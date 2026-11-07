// IEEE 1800-2017 6.7.1 Net declarations with a declaration assignment.
// `wire w = v + 1` is a continuous assignment: w follows every change of v.
module t;
  logic [7:0] v;
  wire  [7:0] w = v + 1;
  initial begin
    v = 5;
    #1 $display("%0d", w);
    v = 9;
    #1 $display("%0d", w);
  end
endmodule
//> 6
//> 10
