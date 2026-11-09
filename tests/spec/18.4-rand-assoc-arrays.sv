// IEEE 1800-2023 18.4: a rand associative array keeps its keys and gets
// new values. foreach binds the index variable to each key (18.5.8.1),
// size() is the number of keys, reductions with `with` fold over the
// values (18.5.8.2), and a foreach over a non-rand associative array reads
// its keys too. When the constraints cannot be met, nothing changes.
//! icarus-skip: Icarus 13 does not support classes
class C;
  rand bit [7:0] aa[int];
  rand byte unsigned ab[bit [3:0]];
  int other[int];
  constraint c { foreach (aa[k]) aa[k] == k + 100; aa.size() == 3;
                 aa.sum() with (int'(item)) == 322;
                 foreach (ab[k]) ab[k] inside {[k*2:k*2+1]};
                 foreach (other[k]) other[k] != aa[k]; }
endclass

module top;
  C c = new;
  int bad;
  initial begin
    c.aa[5] = 0; c.aa[-3] = 0; c.aa[20] = 9;
    c.ab[4'hf] = 0; c.ab[2] = 0;
    c.other[5] = 7;
    repeat (10) begin
      if (!c.randomize()) bad++;
      if (c.aa.num() != 3 || c.aa[-3] != 97 || c.aa[5] != 105 || c.aa[20] != 120) bad++;
      if (c.ab.num() != 2 || c.ab[2] / 2 != 2 || c.ab[15] / 2 != 15) bad++;
    end
    $display("bad %0d", bad);
    c.other[20] = 120;   // now aa[20] cannot be 120
    if (c.randomize()) $display("unexpected success");
    $display("%p", c.aa);
  end
endmodule

//> bad 0
//> '{-3:97, 5:105, 20:120}
