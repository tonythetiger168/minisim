// IEEE 1800-2023 11.5.1 Indexed part-select +: and -: with constant and
// variable start, read and write. Bits outside the vector read x, and a
// partly out-of-range write changes only the bits inside.
module t;
  logic [15:0] a;
  logic [31:0] w;
  int i;
  initial begin
    a = 16'hbeef;
    i = 4;
    $display("%h %h %h %h", a[i +: 8], a[15 -: 4], a[0 +: 4], a[i*2 -: 8]);
    w = 0;
    for (i = 0; i < 4; i++) w[i*8 +: 8] = 8'h10 + i;
    $display("%h", w);
    i = 12;
    $display("%h", a[i +: 8]);
    a[i +: 8] = 8'h00;
    $display("%h", a);
    w[23 -: 8] = 8'hcd;
    $display("%h", w);
  end
endmodule
//> ee b f 77
//> 13121110
//> xb
//> 0eef
//> 13cd1110
