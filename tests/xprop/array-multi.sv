// msim -xprop: blocking writes to an element of a small array (64 elements
// or fewer) chosen at run time. The region saves, puts back and merges every
// element like a variable, so branches that write the same element, or read
// back what they wrote, merge exactly instead of with the old value.
//! args: -xprop
module top;
  logic c, d; logic [1:0] a; logic [7:0] m [4], r; logic [3:0] g [2][2];
  always_comb begin
    for (int i = 0; i < 4; i++) m[i] = 8'h10 * i;
    if (c) begin m[a] = 8'h55; r = m[a]; end
    else begin m[a] = 8'h55; m[a] = m[a] + 8'd1; r = m[a]; end
  end
  always_comb begin
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) g[i][j] = 4'h0;
    case (d)
      1'b0: g[a[0]][a[1]] = 4'h9;
      1'b1: g[a[0]][a[1]] = 4'hb;
    endcase
  end
  initial begin
    a = 2; c = 1; d = 0; #1 $display("%h %h %h %h r=%h g=%h %h %h %h", m[0], m[1], m[2], m[3], r, g[0][0], g[0][1], g[1][0], g[1][1]);
    c = 1'bx; d = 1'bx; #1 $display("%h %h %h %h r=%b g=%b %b %b %b", m[0], m[1], m[2], m[3], r, g[0][0], g[0][1], g[1][0], g[1][1]);
    c = 0; d = 1; #1 $display("%h %h %h %h r=%h g=%h %h %h %h", m[0], m[1], m[2], m[3], r, g[0][0], g[0][1], g[1][0], g[1][1]);
    $finish;
  end
endmodule
//> 00 10 55 30 r=55 g=0 9 0 0
//> 00 10 5X 30 r=010101xx g=0000 10x1 0000 0000
//> 00 10 56 30 r=56 g=0 b 0 0
