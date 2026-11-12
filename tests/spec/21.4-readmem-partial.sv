// IEEE 1800-2023 21.4: the memory argument of $readmemh / $readmemb (and
// $writememh, 21.5) can be a partly indexed multidimensional array that
// leaves a lesser dimensioned array; the lowest dimension given can be a
// slice. Addresses in the file and start / finish then refer to the
// selected array. Run from tests/spec.
//! icarus-skip: Icarus 13 does not take a partly indexed memory
module top;
  logic [7:0] m [0:2][0:3];
  logic [7:0] n [0:1][0:2][0:3];
  logic [7:0] d [3:0][7:4];
  int k;
  initial begin
    foreach (m[i, j]) m[i][j] = 8'(i * 16 + j);
    foreach (n[i, j, l]) n[i][j][l] = 8'hee;
    $writememh("spec_21_4_p4.hex", m[1]);          // 10 11 12 13
    $writememh("spec_21_4_p2.hex", m[2][1:2]);     // 21 22
    $readmemh("spec_21_4_p4.hex", n[1][2]);
    $readmemh("spec_21_4_p2.hex", n[0][1][2:3]);
    k = 1;
    $readmemh("spec_21_4_p2.hex", n[k][0][k+:2]);
    $readmemh("spec_21_4_p4.hex", n[0][k-1], 3, 0);  // descending: 13 12 11 10 into [0:3]
    foreach (n[i, j]) begin
      for (int l = 0; l < 4; l++) $write("%h ", n[i][j][l]);
      $display("| %0d%0d", i, j);
    end
    $writememb("spec_21_4_pb.bin", m[k][2:3]);
    $readmemb("spec_21_4_pb.bin", d[2][5:4]);   // loads from the low address up: 4, then 5
    $display("%h %h %h", d[2][5], d[2][4], d[2][6]);
  end
endmodule
//> 13 12 11 10 | 00
//> ee ee 21 22 | 01
//> ee ee ee ee | 02
//> ee 21 22 ee | 10
//> ee ee ee ee | 11
//> 10 11 12 13 | 12
//> 13 12 xx
