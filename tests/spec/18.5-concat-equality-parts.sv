// IEEE 1800-2023 18.5, 11.4.12: a constraint that equates a concatenation
// with a value, where one part is solved first. Each part has to equal its
// bits of the value, so a low part picked earlier fixes what the high part
// may be; the solver has to see that rather than search the high part's
// whole range.
//! icarus-skip: Icarus 13 does not support constraints
class split_key;
  rand bit [63:0] h;
  rand bit [3:0] l;
  rand bit [7:0] t;
  constraint c { t == {h[3:0], l}; t < 16; solve t, l before h; }
endclass
class two_halves;
  rand bit [63:0] hi_w, lo_w;
  rand bit [7:0] tag;
  constraint c { hi_w[63:56] == 8'h3c; tag == {hi_w[3:0], lo_w[63:60]}; tag < 12; }
endclass
module top;
  initial begin
    split_key k;
    two_halves w;
    int bad;
    k = new;
    w = new;
    bad = 0;
    repeat (20) begin
      if (!k.randomize()) bad++;
      if (k.h[3:0] != 0 || k.t != {4'b0, k.l}) bad++;
      if (!w.randomize()) bad++;
      if (w.hi_w[63:56] != 8'h3c || w.hi_w[3:0] != 0 || w.tag != {w.hi_w[3:0], w.lo_w[63:60]} || w.tag >= 12) bad++;
    end
    $display("bad %0d", bad);
  end
endmodule
//> bad 0
