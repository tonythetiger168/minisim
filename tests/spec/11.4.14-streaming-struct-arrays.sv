// IEEE 1800-2023 11.4.14: streaming with unpacked aggregates and containers.
// A fixed-size array of unpacked structs is a bit-stream type (6.24.3): as an
// operand its elements are streamed left index first, each struct member by
// member (11.4.14.1), and as a target it is filled the same way (11.4.14.3).
// A queue may be the source of an unpack into several fixed-size targets, and
// a stream may be assigned to a multidimensional unpacked array, procedurally
// or by a continuous assignment.
//! icarus-skip: Icarus 13 does not support streaming concatenation
module top;
  typedef struct { logic [7:0] hi; logic [7:0] lo; } pair_t;
  pair_t pr[2], pl[2];
  logic [31:0] word = 32'hCAFE_F00D, back;
  int a, b;
  byte bq[$];
  bit grid[2][3];
  logic [5:0] six = 6'b110_001, flat;
  logic wired[3];
  assign wired = {<<{six[2:0]}};
  initial begin
    {>>{pr}} = word;
    {<<8{pl}} = word;
    $display("%h %h %h %h", pr[0].hi, pr[0].lo, pr[1].hi, pr[1].lo);
    $display("%h %h %h %h", pl[0].hi, pl[0].lo, pl[1].hi, pl[1].lo);
    back = {>>{pr}};
    $display("%h", back);
    back = {<<8{pl}};
    $display("%h", back);

    a = 32'h1122_3344; b = 32'h5566_7788;
    bq = {<<8{a, b}};
    $display("%0d %h %h", bq.size(), bq[0], bq[7]);
    a = 0; b = 0;
    {<<8{a, b}} = bq;
    $display("%h %h", a, b);
    bq.push_back(8'hEE);
    {>>{a}} = bq;
    $display("%h", a);

    grid = {>>{six}};
    $display("%p", grid);
    grid = {>>{6'b011_010}};
    $display("%p", grid);
    {>>{flat}} = grid;
    $display("%b", flat);
    #1 $display("%p", wired);
  end
endmodule
//> ca fe f0 0d
//> 0d f0 fe ca
//> cafef00d
//> cafef00d
//> 8 88 11
//> 11223344 55667788
//> 88776655
//> '{'{1, 1, 0}, '{0, 0, 1}}
//> '{'{0, 1, 1}, '{0, 1, 0}}
//> 011010
//> '{1, 0, 0}
