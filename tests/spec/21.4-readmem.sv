// IEEE 1800-2023 21.4 Loading memory array data from a file: comments, @address,
// underscores, start/finish arguments, a descending load, x digits, and
// 21.4.2 (a 2-state element reads x as 0). Run from the tests/spec directory.
module t;
  logic [7:0] m [0:7];
  logic [3:0] b [1:4];
  bit   [3:0] b2 [1:4];
  logic [7:0] d [0:7];
  initial begin
    $readmemh("data/mem.hex", m);
    $display("%h %h %h %h %h %h", m[0], m[1], m[2], m[3], m[4], m[5]);
    $readmemb("data/mem.bin", b);
    $readmemb("data/mem.bin", b2);
    $display("%b %b %b %b", b[1], b[2], b[3], b2[3]);
    $readmemh("data/mem.hex", d, 5, 0);
    $display("%h %h", d[5], d[4]);
  end
endmodule
//> 0a 0b xx ff 10 05
//> 0001 0010 xx01 0001
//> 0a 0b
