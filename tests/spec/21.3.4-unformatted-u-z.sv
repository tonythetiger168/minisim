// IEEE 1800-2023 21.2.1.1 / 21.3.4.3: %u writes 2-value binary data and %z
// 4-value data in 32-bit words, least significant word first; $fscanf with
// %u / %z reads them back.
module t;
  reg [39:0] a, b;
  reg [7:0] c, d;
  integer f, r;
  initial begin
    a = 40'h12_3456_789a;
    c = 8'b10xz_01zx;
    f = $fopen("unformatted_uz.bin", "wb");
    $fwrite(f, "%u%z", a, c);
    $fclose(f);
    f = $fopen("unformatted_uz.bin", "rb");
    r = $fscanf(f, "%u%z", b, d);
    $display("%0d %h %b", r, b, d);
    r = $fscanf(f, "%u", b);
    $display("%0d", r);
    $fclose(f);
  end
endmodule
//> 2 123456789a 10xz01zx
//> -1
