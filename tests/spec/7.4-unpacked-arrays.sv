// IEEE 1800-2023 7.4 Packed and unpacked arrays: element read and write with
// constant and variable indices, descending ranges, two dimensions, a bit of
// an element, and 7.4.6 (an out-of-range read gives X; a write is ignored).
module t;
  logic [7:0] mem [0:3];
  logic [3:0] rev [3:0];
  logic [7:0] grid [2][3];
  int i, j;
  initial begin
    for (i = 0; i < 4; i++) mem[i] = 8'h10 + i;
    mem[2] = 8'hAA;
    $display("%h %h %h %h", mem[0], mem[1], mem[2], mem[3]);
    i = 3;
    rev[i] = 4'd9;
    rev[0] = 4'd1;
    $display("%0d %0d", rev[3], rev[0]);
    for (i = 0; i < 2; i++)
      for (j = 0; j < 3; j++)
        grid[i][j] = i * 10 + j;
    $display("%0d %0d %0d", grid[0][0], grid[1][2], grid[i-1][j-2]);
    mem[1][0] = 1'b0;
    $display("%h %b", mem[1], mem[3][1:0]);
    i = 7;
    mem[i] = 8'hFF;
    $display("%h %h", mem[i], mem[0]);
  end
endmodule
//> 10 11 aa 13
//> 9 1
//> 0 12 11
//> 10 11
//> xx 10
