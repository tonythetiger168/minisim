// IEEE 1800-2023 12.7.1, 9.2.2.2: for loops with a constant trip count in
// always_comb and always @*: one bit per pass (a bit reversal), a decoder
// that compares the loop variable, an indexed part-select per pass, a loop
// that counts down, and nested loops. msim unrolls them; the results must be
// those of running the loops.
module top;
  logic [31:0] a, rev;
  logic [4:0]  wa;
  logic        we;
  logic [31:0] dec;
  logic [7:0]  bytes [0:3];
  logic [31:0] packed_b;
  logic [3:0]  first_one;
  logic [15:0] mat;
  always_comb begin
    for (int unsigned i = 0; i < 32; i++) rev[i] = a[31 - i];
  end
  always_comb begin
    for (int unsigned i = 0; i < 32; i++) dec[i] = (wa == 5'(i)) ? we : 1'b0;
  end
  always @* begin
    for (int i = 0; i < 4; i++) packed_b[i*8 +: 8] = bytes[i] ^ 8'(i);
  end
  always_comb begin
    first_one = 4'hf;
    for (int i = 15; i >= 0; i--)
      if (a[i]) first_one = 4'(i);
  end
  always_comb begin
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) mat[r*4 + c] = a[r] & a[c + 4];
  end
  initial begin
    a = 32'h0000_0001; wa = 0; we = 1;
    foreach (bytes[i]) bytes[i] = 8'h10 * i;
    #1 $display("%h %h %h %h %h", rev, dec, packed_b, first_one, mat);
    a = 32'hf0f0_1230; wa = 31; bytes[2] = 8'hff;
    #1 $display("%h %h %h %h %h", rev, dec, packed_b, first_one, mat);
    a = 32'h8000_00a5; we = 0;
    #1 $display("%h %h %h %h %h", rev, dec, packed_b, first_one, mat);
  end
endmodule

//> 80000000 00000001 33221100 0 0000
//> 0c480f0f 80000000 33fd1100 4 0000
//> a5000001 00000000 33fd1100 0 0a0a
