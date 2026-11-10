// IEEE 1800-2023 11.4.14: Streaming operators pack and reverse bits or
// slices (11.4.14), as a value, into a wider target (left-aligned), from
// fixed-size arrays, nested, and as an assignment target (11.4.14.3).
//! icarus-skip: Icarus 13 does not support streaming concatenation
module top;
  logic [5:0] s = 6'b110101, s2;
  logic [7:0] m [4] = '{8'h11, 8'h22, 8'h33, 8'h44};
  logic [31:0] w;
  logic [3:0] n1, n2;
  logic [11:0] big;

  logic [7:0] a = 8'b0000_0001, b;
  logic [15:0] h = 16'h1234, k;
  initial begin
    b = {<<{a}};       $display("%b", b);
    k = {<<4{h}};      $display("%h", k);
    k = {<<8{h}};      $display("%h", k);
    k = {>>{h}};       $display("%h", k);
    {>>{b, a}} = h;    $display("%h %h", b, a);
  end
  initial begin
    #1;
    s2 = {<<4{s}};  $display("%b", s2);
    w = {>>{m}};    $display("%h", w);
    w = {<<8{m}};   $display("%h", w);
    big = {>>{4'hA, 4'hB}};  $display("%h", big);
    {<<4{n1, n2}} = 8'hC3;   $display("%h %h", n1, n2);
    {>>{n1, n2}} = 12'hDEF;  $display("%h %h", n1, n2);
    w = {<<{8'b1}};  $display("%h", w);
    w = {<<byte{16'hABCD}};  $display("%h", w);
    s2 = {<<2{{<<{4'b1101}}}}; $display("%b", s2);
    {<<4{n1, s[1:0]}} = 6'b010111; $display("%b %b", n1, s[1:0]);
  end
endmodule

//> 10000000
//> 4321
//> 3412
//> 1234
//> 12 34
//> 010111
//> 11223344
//> 44332211
//> ab0
//> 3 c
//> d e
//> 80000000
//> cdab0000
//> 111000
//> 1101 01
