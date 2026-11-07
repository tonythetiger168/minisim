// IEEE 1800-2023 11.5.1 Vector bit-select addressing: a bit chosen by a
// variable, read and written; a write to a bit out of range or at an x index
// is ignored, and such a read gives x.
module t;
  logic [7:0] v;
  int i;
  logic [3:0] x4;
  initial begin
    v = 0;
    for (i = 0; i < 8; i += 2) v[i] = 1'b1;
    $display("%b", v);
    i = 9;
    v[i] = 1'b0;
    $display("%b %b", v, v[i]);
    x4 = 4'bxxxx;
    v[x4] = 1'b0;
    $display("%b", v);
  end
endmodule
//> 01010101
//> 01010101 x
//> 01010101
