// An inlined function's automatic variables (a Keccak-style theta step:
// arrays written bit by bit, loop indices) belong to the one process that
// calls it. msim -compile stores them (Sim::poke, in place for up to 64
// bits of a wide one) without a change event (msim.cpp quietTemps).
module quiettmp;
  localparam int W = 16;
  typedef logic [4:0][4:0][W-1:0] box_t;
  typedef logic [4:0][W-1:0] plane_t;
  function automatic box_t theta(box_t state);
    plane_t c; plane_t d; box_t result;
    for (int x = 0; x < 5; x++)
      for (int z = 0; z < W; z++)
        c[x][z] = state[x][0][z] ^ state[x][1][z] ^ state[x][2][z] ^ state[x][3][z] ^ state[x][4][z];
    for (int x = 0; x < 5; x++) begin
      int i1, i2; i1 = (x == 0) ? 4 : x-1; i2 = (x == 4) ? 0 : x+1;
      for (int z = 0; z < W; z++) begin int iz; iz = (z == 0) ? W-1 : z-1; d[x][z] = c[i1][z] ^ c[i2][iz]; end
    end
    for (int x = 0; x < 5; x++) for (int y = 0; y < 5; y++) for (int z = 0; z < W; z++) result[x][y][z] = state[x][y][z] ^ d[x][z];
    return result;
  endfunction
  logic clk = 0;
  box_t st = '0, th;
  assign th = theta(st);
  logic [31:0] acc = 0;
  always @(posedge clk) begin st <= th ^ {25{acc[15:0] + 16'h1}}; acc <= acc * 31 + th[2][3] + th[4][0]; end
  initial begin
    repeat (2000) begin #5 clk = 1; #5 clk = 0; end
    $display("acc=%h th=%h", acc, th[1]);
    $finish;
  end
endmodule
