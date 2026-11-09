// IEEE 1800-2023 11.4.14: Streaming operators pack and reverse bits or
// slices (11.4.14).
//! xfail: streaming operators are errors
//! icarus-skip: Icarus 13 does not support streaming concatenation
module top;
  logic [7:0] a = 8'b0000_0001, b;
  logic [15:0] h = 16'h1234, k;
  initial begin
    b = {<<{a}};       $display("%b", b);
    k = {<<4{h}};      $display("%h", k);
    k = {<<8{h}};      $display("%h", k);
    k = {>>{h}};       $display("%h", k);
    {>>{b, a}} = h;    $display("%h %h", b, a);
  end
endmodule

//> 10000000
//> 4321
//> 3412
//> 1234
//> 12 34
