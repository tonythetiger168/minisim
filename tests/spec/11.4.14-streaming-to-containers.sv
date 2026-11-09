// IEEE 1800-2023 11.4.14.3: a streaming concatenation of one dynamic array as
// the target takes as many elements as the source has slices, and a
// streaming concatenation can be the source of another one.
//! icarus-skip: Icarus 13 does not support streaming into dynamic arrays
module top;
  bit arr[];
  byte unsigned b[];
  logic [15:0] w;
  logic [31:0] d;
  initial begin
    {<< bit {arr}} = 8'b1100_0101;
    $display("%0d %b%b%b%b%b%b%b%b", arr.size(), arr[7], arr[6], arr[5], arr[4], arr[3], arr[2], arr[1], arr[0]);
    {>> byte {b}} = 24'hA1B2C3;
    $display("%0d %h %h %h", b.size(), b[0], b[1], b[2]);
    d = 32'h11223344;
    {>> {d}} = {<< byte {d}};
    $display("%h", d);
  end
endmodule

//> 8 11000101
//> 3 a1 b2 c3
//> 44332211
