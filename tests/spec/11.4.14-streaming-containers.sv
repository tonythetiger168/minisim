// IEEE 1800-2023 11.4.14: streaming with queues and dynamic arrays: packing a
// queue into a vector (left-aligned), unpacking a vector into a queue (the
// last element padded on the right), << on bytes, and an empty queue.
//! icarus-skip: Icarus 13 does not support streaming concatenation
module top;
  byte unsigned q[$], p[$];
  logic [31:0] w;
  logic [23:0] w3;
  logic [3:0] n [$];
  initial begin
    w = 32'hDEADBEEF;
    q = {>>{w}};      $display("%0d %h %h %h %h", q.size(), q[0], q[1], q[2], q[3]);
    q = {<<8{w}};     $display("%h %h", q[0], q[3]);
    w = 0;
    w = {>>{q}};      $display("%h", w);
    q.push_back(8'h11);
    w3 = {>>{q}};     $display("%h", w3);
    w = {<<8{q}};     $display("%h", w);
    p = {>>{q, 8'hAA}}; $display("%0d %h", p.size(), p[5]);
    n = {>>{12'hABC, 2'b11}}; $display("%0d %h %h", n.size(), n[0], n[3]);
    q = {};
    w = {>>{q}};      $display("%h", w);
  end
endmodule
//> 4 de ad be ef
//> ef de
//> efbeadde
//> efbead
//> 11deadbe
//> 6 aa
//> 4 a c
//> 00000000
