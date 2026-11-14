// IEEE 1800-2023 9.4.2: edge is a posedge or a negedge, so a change between
// x and z is not an edge.
module t;
  logic s;
  int n;
  always @(edge s) n++;
  initial begin
    n = 0;
    s = 1'bx;
    #1 s = 1'bz;
    #1 s = 1'bx;
    #1 $display("%0d", n);
    s = 1'b0;
    #1 s = 1'bz;
    #1 s = 1'b1;
    #1 $display("%0d", n);
  end
endmodule
//> 0
//> 3
