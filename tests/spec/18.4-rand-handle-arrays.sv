// IEEE 1800-2023 18.4, 18.5.8.1: a rand dynamic array of class handles
// randomizes each non-null element's object with its parent; a foreach
// constraint over a non-rand queue of handles reads the elements' members,
// and over a non-rand dynamic array of integers excludes values.
//! icarus-skip: Icarus 13 does not support classes
class Ext;
  rand bit [7:0] k;
  constraint c { k < 10; }
endclass
class Region;
  bit [15:0] lo, hi;
  function new(bit [15:0] a, bit [15:0] b); lo = a; hi = b; endfunction
endclass
class Payload;
  rand bit [7:0] data[];
  rand bit [7:0] be[];
  rand int unsigned len;
  rand Ext exts[];
  constraint c { len inside {[1:8]}; data.size() == len; be.size() == len;
                 foreach (be[i]) be[i] inside {0, 8'hff}; }
endclass
class Alloc;
  rand bit [15:0] start;
  bit [15:0] len = 16;
  Region in_use[$];
  int skip[];
  constraint c { start <= 255 - len + 1; }
  constraint no_overlap { foreach (in_use[i]) !(start <= in_use[i].hi && start + len - 1 >= in_use[i].lo); }
  constraint no_skip { foreach (skip[i]) start != skip[i]; }
endclass

module top;
  Payload p = new;
  Alloc a = new;
  Region r;
  int bad;
  initial begin
    p.exts = new[3];
    foreach (p.exts[i]) if (i != 1) p.exts[i] = new;
    r = new(0, 99); a.in_use.push_back(r);
    r = new(150, 239); a.in_use.push_back(r);
    a.skip = '{100, 101, 102};
    repeat (40) begin
      if (!p.randomize()) bad++;
      if (p.data.size() != p.len || p.be.size() != p.len || p.len < 1 || p.len > 8) bad++;
      foreach (p.be[i]) if (p.be[i] != 0 && p.be[i] != 8'hff) bad++;
      if (p.exts[0].k >= 10 || p.exts[2].k >= 10 || p.exts[1] != null) bad++;
      if (!a.randomize()) bad++;
      if (a.start < 103 || (a.start > 134 && a.start != 240)) bad++;
    end
    $display("bad %0d", bad);
  end
endmodule

//> bad 0
