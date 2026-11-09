// IEEE 1800-2023 9.4.2 Event control: an edge or value change on an
// expression such as a bit select, a member, or an operator result. An
// edge is detected on the expression's least significant bit.
module t;
  logic [1:0] clks = 2'b00;
  logic a = 0, b = 0;
  int n0 = 0, n1 = 0, nab = 0, nneg = 0;
  always @(posedge clks[0]) n0++;
  always @(posedge clks[1]) n1++;
  always @(a & b) nab++;
  always @(negedge clks[1]) nneg++;
  initial begin
    #1 clks = 2'b01;
    #1 clks = 2'b11;
    #1 clks = 2'b10;
    #1 clks = 2'b01;
    #1 a = 1;
    #1 b = 1;
    #1 a = 0;
    #1 $display("%0d %0d %0d %0d", n0, n1, nab, nneg);
  end
endmodule
//> 2 1 2 1
