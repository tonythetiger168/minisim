// IEEE 1800-2023 18.5.3, 18.5.8.1: a dist on each element of a fixed-size
// rand array (in a foreach, or naming the element) weights that element's
// values as it would a scalar's: 0 := 5 against 1..9 := 1 each gives 0 about
// 5/14 of the time.
//! icarus-skip: Icarus does not support constraints
class F;
  rand bit [3:0] a[4];
  constraint c { foreach (a[i]) a[i] dist { 4'd0 := 5, [4'd1 : 4'd9] := 1 }; }
endclass
class G;
  rand bit [3:0] m[2][2];
  constraint c { m[1][0] dist { 4'd15 :/ 3, [4'd0 : 4'd14] :/ 1 }; }
endclass
module t;
  F f = new;
  G g = new;
  int zero[4], fifteen, bad;
  initial begin
    repeat (2800) begin
      if (!f.randomize()) bad++;
      foreach (f.a[i]) begin
        if (f.a[i] > 9) bad++;
        zero[i] += f.a[i] == 0;
      end
      if (!g.randomize()) bad++;
      fifteen += g.m[1][0] == 15;
    end
    // expected 1000 each and 2100; a uniform draw gives 280 and 175
    foreach (zero[i]) $display("a[%0d] %s", i, zero[i] > 850 && zero[i] < 1150 ? "weighted" : "WRONG");
    $display("m[1][0] %s, bad %0d", fifteen > 1950 && fifteen < 2250 ? "weighted" : "WRONG", bad);
  end
endmodule
//> a[0] weighted
//> a[1] weighted
//> a[2] weighted
//> a[3] weighted
//> m[1][0] weighted, bad 0
