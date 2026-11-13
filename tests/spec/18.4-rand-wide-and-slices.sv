// IEEE 1800-2023 18.4, 18.5, 20.9: rand variables wider than 64 bits, also
// as the elements of a dynamic array or queue, with constraints on part
// selects of them; and constraints on part selects and $countones of
// narrower variables next to other rand variables.
//! icarus-skip: Icarus 13 does not support constraints
class wide_item;
  rand bit [127:0] word;
  rand bit [127:0] data[];
  rand bit [95:0] q[$];
  rand bit [7:0] tag;
  constraint c_size { data.size() == 4; q.size() == 2; }
  constraint c_bits {
    word[127:120] == 8'ha5;
    word[67:60] == tag;
    tag < 16;
    foreach (data[i]) { data[i][127:120] == 8'(i); data[i][3:0] == 4'hc; }
    foreach (q[i]) q[i][95:64] == 32'hcafe_0000 + i;
  }
endclass
class key_item;
  rand bit [39:0] ksv;
  rand int unsigned n;
  rand bit e;
  rand bit [31:0] r;
  constraint c { $countones(ksv) == 20; r[31:24] == 8'h5a; r[7:0] > 8'hf0; }
endclass
module top;
  initial begin
    wide_item w;
    key_item k;
    int bad;
    w = new;
    k = new;
    bad = 0;
    repeat (5) begin
      if (!w.randomize()) bad++;
      if (w.word[127:120] != 8'ha5 || w.word[67:60] != w.tag || w.tag >= 16) bad++;
      if (w.data.size() != 4 || w.q.size() != 2) bad++;
      foreach (w.data[i]) if (w.data[i][127:120] != i || w.data[i][3:0] != 4'hc) bad++;
      foreach (w.q[i]) if (w.q[i][95:64] != 32'hcafe_0000 + i) bad++;
      if (!k.randomize()) bad++;
      if ($countones(k.ksv) != 20 || k.r[31:24] != 8'h5a || k.r[7:0] <= 8'hf0) bad++;
    end
    $display("bad %0d", bad);
    $display("top %h %h", w.data[3][127:120], w.q[1][95:64]);
  end
endmodule
//> bad 0
//> top 03 cafe0001
