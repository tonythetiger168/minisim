// IEEE 1800-2023 18.4, 18.5.8.1: rand dynamic arrays and queues. The size
// is solved first (array.size() in a constraint), then the elements; a
// foreach constraint applies to every element, also comparing neighbours;
// an array with no size constraint keeps its size; an enum array takes
// only named values.
//! icarus-skip: Icarus 13 does not support classes
typedef enum {RED, GREEN, BLUE} color_t;
class Pkt;
  rand bit [7:0] data[];
  rand int q[$];
  rand color_t cs[];
  rand int len;
  rand byte keep[];
  constraint c_len { len inside {[2:5]}; data.size() == len; }
  constraint c_q { q.size() inside {[1:4]}; foreach (q[i]) { q[i] inside {[0:20]}; if (i > 0) q[i] > q[i-1]; } }
  constraint c_cs { cs.size() == 3; }
  constraint c_d { foreach (data[i]) data[i] < 100; data[0] == 7; }
  constraint c_keep { foreach (keep[i]) keep[i] == i; }
  function new(); keep = new[3]; endfunction
endclass

module top;
  Pkt p = new;
  int bad;
  initial begin
    repeat (50) begin
      if (!p.randomize()) bad++;
      if (p.data.size() != p.len || p.len < 2 || p.len > 5 || p.data[0] != 7) bad++;
      foreach (p.data[i]) if (p.data[i] >= 100) bad++;
      if (p.q.size() < 1 || p.q.size() > 4) bad++;
      foreach (p.q[i]) if (p.q[i] > 20 || (i > 0 && p.q[i] <= p.q[i-1])) bad++;
      if (p.cs.size() != 3) bad++;
      foreach (p.cs[i]) if (!(p.cs[i] inside {RED, GREEN, BLUE})) bad++;
      if (p.keep.size() != 3 || p.keep[2] != 2) bad++;
    end
    $display("bad %0d", bad);
    if (!p.randomize() with { q.size() == 2; q[1] == 9; data.size() == 4; }) $display("fail");
    $display("%0d %0d %0d %0d", p.q.size(), p.q[1], p.data.size(), p.len);
  end
endmodule

//> bad 0
//> 2 9 4 4
