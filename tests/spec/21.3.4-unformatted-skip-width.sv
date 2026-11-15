// IEEE 1800-2023 21.3.4.3: $fscanf %*u / %*z read binary data and store
// nothing: one 32-bit word by default. A width in the conversion (%16u) is a
// number of bits, read as whole 32-bit words and cut to that many bits.
module t;
  reg [31:0] a, b, r32;
  reg [63:0] r64;
  integer f, n;
  initial begin
    a = 32'hdead_beef;
    b = 32'h1234_5678;
    f = $fopen("unformatted_skip.bin", "wb");
    $fwrite(f, "%u%u%u", a, b, a);
    $fclose(f);
    f = $fopen("unformatted_skip.bin", "rb");
    n = $fscanf(f, "%*u%16u", r32);
    $display("%0d %h", n, r32);
    n = $fscanf(f, "%32u", r64);
    $display("%0d %h", n, r64);
    n = $fscanf(f, "%*u%u", r32);
    $display("%0d", n);
    $fclose(f);
  end
endmodule
//> 1 00005678
//> 1 00000000deadbeef
//> -1
