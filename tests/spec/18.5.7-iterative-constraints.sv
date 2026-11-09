// IEEE 1800-2023 18.5.7: foreach constraints over array elements, with the
// index usable in the constraint, and multidimensional arrays (18.5.7.1).
// Array reduction methods in constraints (18.5.7.2).
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class F;
    rand bit [7:0] a[5];
    rand bit [3:0] m[2][3];
    rand bit [7:0] s[4];
    constraint inc { foreach (a[i]) if (i > 0) a[i] > a[i-1]; a[0] >= 10; }
    constraint grid { foreach (m[i, j]) m[i][j] == i * 3 + j; }
    constraint total { s.sum() with (int'(item)) == 100; foreach (s[i]) s[i] <= 40; }
  endclass
  F f;
  int bad, ok = 1;
  initial begin
    f = new;
    repeat (50) begin
      ok &= f.randomize();
      for (int i = 1; i < 5; i++) if (f.a[i] <= f.a[i-1]) bad++;
      if (f.a[0] < 10) bad++;
      foreach (f.m[i, j]) if (f.m[i][j] != i * 3 + j) bad++;
      if (f.s[0] + f.s[1] + f.s[2] + f.s[3] != 100) bad++;
      foreach (f.s[i]) if (f.s[i] > 40) bad++;
    end
    $display("%0d %0d", ok, bad);
  end
endmodule

//> 1 0
