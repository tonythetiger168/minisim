//! icarus-skip: Icarus has no streaming assignment targets
// IEEE 1800-2023 11.4.14.3: the source of a streaming assignment is any
// bit-stream type, here a fixed-size unpacked array: its elements stream
// left to right, the leftmost element first.
module top;
  bit arr[6] = '{1, 1, 0, 1, 0, 0};
  logic [1:0] pairs[3] = '{2'b10, 2'b01, 2'b11};
  bit [5:0] v;
  bit [2:0] hi, lo;
  initial begin
    {>>{v}} = arr;
    $display("%b", v);
    {<<{v}} = arr;
    $display("%b", v);
    {>>{hi, lo}} = pairs;
    $display("%b %b", hi, lo);
    {<<2{hi, lo}} = pairs;
    $display("%b %b", hi, lo);
  end
endmodule
//> 110100
//> 001011
//> 100 111
//> 110 110
