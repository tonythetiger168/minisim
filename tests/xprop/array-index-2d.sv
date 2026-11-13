// msim -xprop: a write to an element of a two-dimensional array whose
// indexes have unknown bits. Every element they may name takes the merge
// of its present value and the new one; a known index limits the others.
//! args: -xprop
module top;
  logic clk = 0, we;
  logic [1:0] r; logic c;
  logic [7:0] d, m [4][2], k [0:1][3:2];
  always @(posedge clk) if (we) m[r][c] <= d;
  always_comb begin
    for (int i = 0; i < 2; i++) for (int j = 2; j < 4; j++) k[i][j] = 8'h10 * i + j;
    k[r[0]][c ? 3 : 2] = 8'hff;
  end
  task show;
    $display("m=%h %h %h %h %h %h %h %h k=%h %h %h %h", m[0][0], m[0][1], m[1][0], m[1][1],
             m[2][0], m[2][1], m[3][0], m[3][1], k[0][2], k[0][3], k[1][2], k[1][3]);
  endtask
  initial begin
    for (int i = 0; i < 4; i++) for (int j = 0; j < 2; j++) m[i][j] = 8'h50 + 2 * i + j;
    we = 1; r = 2'b0x; c = 1; d = 8'h5f;
    #1 clk = 1; #1 clk = 0; show;
    r = 2'b10; c = 1'bx; d = 8'h50; #1 clk = 1; #1 clk = 0; show;
    r = 2'bx1; c = 0; d = 8'h00; #1 clk = 1; #1 clk = 0; show;
    r = 2'b11; c = 1; d = 8'h77; #1 clk = 1; #1 clk = 0; show;
    $finish;
  end
endmodule
//> m=50 5X 52 5X 54 55 56 57 k=02 xX 12 XX
//> m=50 5X 52 5X 5X 5X 56 57 k=xX xX 12 13
//> m=50 5X XX 5X 5X 5X XX 57 k=02 03 ff 13
//> m=50 5X XX 5X 5X 5X XX 77 k=02 03 12 ff
