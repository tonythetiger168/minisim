// IEEE 1800-2023 20.7: Array query functions on dynamic arrays, queues and
// associative arrays read their current state; integer types count as one
// packed dimension; a type argument works; a dimension out of range gives 'x.
//! icarus-skip: Icarus 13 does not support array queries on dynamic arrays
module top;
  typedef logic [16:1] Word;
  Word ram[0:9];
  int d[];
  byte q[$];
  int aa[int];
  int k;
  logic [3:0][2:1] n [1:5][2:8];
  initial begin
    $display("%0d %0d %0d", $size(Word), $size(ram, 2), $dimensions(ram));
    $display("%0d %0d %0d %0d", $dimensions(n), $unpacked_dimensions(n), $left(n, 3), $size(n, 4));
    $display("%0d %0d %0d %0d", $dimensions(int), $left(int), $right(byte), $dimensions(logic));
    $display("%0d %0d", $size(d), $right(d));
    d = new[4];
    $display("%0d %0d %0d %0d %0d %0d", $size(d), $left(d), $right(d), $low(d), $high(d), $increment(d));
    q.push_back(1); q.push_back(2); q.push_back(3);
    $display("%0d %0d %0d", $size(q), $high(q), $dimensions(q));
    $display("%0d %0d %0d", $size(aa), $low(aa) === 32'bx, $right(aa));
    aa[-5] = 1; aa[40] = 2; aa[7] = 3;
    $display("%0d %0d %0d %0d", $size(aa), $low(aa), $high(aa), $left(aa));
    k = 3;
    $display("%0d %0d %0d", $size(ram, k) === 32'bx, $size(ram, k - 1), $left(n, k + 1));
  end
endmodule

//> 16 16 2
//> 4 2 3 2
//> 1 31 0 1
//> 0 -1
//> 4 0 3 0 3 -1
//> 3 2 2
//> 0 1 2147483647
//> 3 -5 40 0
//> 1 16 2
