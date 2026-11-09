// IEEE 1800-2023 18.5.3: dist. := gives the weight to each value of a range,
// :/ divides the weight over the range; a value with weight 0 never comes
// up; a dist is also a constraint (the value is in one of the ranges).
//! icarus-skip: Icarus 13 does not support dist
module top;
  class D;
    rand bit [7:0] a, b;
    constraint ca { a dist { 0 := 3, [1:3] := 1, 4 := 0 }; }
    constraint cb { b dist { 10 :/ 1, [20:29] :/ 1 }; }
  endclass
  D d;
  int na[5], b10, bIn, bad;
  initial begin
    d = new;
    repeat (2000) begin
      void'(d.randomize());
      if (d.a > 4 || d.a == 4) bad++; else na[d.a]++;
      if (d.b == 10) b10++;
      else if (d.b >= 20 && d.b <= 29) bIn++;
      else bad++;
    end
    // a: weights 3:1:1:1, so 0 comes up half the time; b: 10 half the time.
    $display("%0d %0d %0d %0d", bad, na[0] > 850 && na[0] < 1150, na[1] > 230 && na[1] < 440, b10 > 850 && b10 < 1150);
  end
endmodule

//> 0 1 1 1
