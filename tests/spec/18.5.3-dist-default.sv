// IEEE 1800-2023 18.5.3: `default :/ w` in a dist gives weight w, divided
// equally, to the values no other item names; without it those values
// cannot be taken. Counts over 4000 draws with generous bounds.
//! icarus-skip: Icarus 13 does not support classes
class C;
  rand bit [3:0] x;
  rand bit [3:0] y;
  constraint cx { x dist { 1 := 50, [2:3] :/ 20, default :/ 30 }; }
  constraint cy { y dist { 0 := 1, 15 := 1, default :/ 0 }; }
endclass

module top;
  C c = new;
  int hx[16], hy[16], other, bad;
  initial begin
    repeat (4000) begin
      if (!c.randomize()) bad++;
      hx[c.x]++;
      hy[c.y]++;
    end
    for (int i = 4; i < 16; i++) other += hx[i];
    other += hx[0];
    // expected: x=1 2000, x=2 and x=3 400 each, the 13 others 1200 in all (about 92 each)
    if (hx[1] < 1800 || hx[1] > 2200) bad++;
    if (hx[2] < 300 || hx[2] > 500 || hx[3] < 300 || hx[3] > 500) bad++;
    if (other < 1000 || other > 1400) bad++;
    for (int i = 4; i < 16; i++) if (hx[i] < 40 || hx[i] > 160) bad++;
    if (hy[0] + hy[15] != 4000 || hy[0] < 1800 || hy[0] > 2200) bad++;
    $display("bad %0d", bad);
  end
endmodule

//> bad 0
