// IEEE 1800-2023 21.4.3: $readmemh and $writememh with a multidimensional
// memory. Each address of the file picks a word of the highest dimension;
// its lower-dimension words follow in row-major order, every dimension
// from its low address up, whatever the direction it was declared with.
// Also 6.6.3: an array of
// wor nets, each element resolving on its own. Run from tests/spec.
//! icarus-skip: Icarus 13 loads multidimensional memories in another order
module top;
  logic [7:0] m [2:0][0:1][3:2];
  logic [7:0] back [0:2][0:1][2:3];
  wor [3:0] w [0:1];
  assign w[0] = 4'b0001;
  assign w[0] = 4'b0100;
  assign w[1] = 4'b1000;
  initial begin
    $readmemh("data/mem3d.hex", m);
    foreach (m[i, j, k]) $write("%0d%0d%0d=%h ", i, j, k, m[i][j][k]);
    $display("");
    $writememh("spec_21_4_3.hex", m);
    $readmemh("spec_21_4_3.hex", back);
    $display("%h %h %h", back[0][0][2], back[1][1][3], back[2][0][3]);
    #1 $display("%b %b", w[0], w[1]);
  end
endmodule
//> 203=21 202=20 213=23 212=22 103=11 102=10 113=13 112=12 003=01 002=00 013=03 012=02 
//> 00 13 21
//> 0101 1000
