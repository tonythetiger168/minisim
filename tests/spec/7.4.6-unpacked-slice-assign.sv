// IEEE 1800-2023 7.4.3, 7.4.6: a slice of an unpacked array may be written
// whole, with a constant range or an indexed +: / -: one; the slice's left
// element takes the value's left one, and the value is read before any
// element is written.
module t;
  int a[0:7];
  int b[7:0];
  int i = 2;
  initial begin
    a[i+:3] = '{10, 11, 12};
    b[5-:3] = '{1, 2, 3};
    $display("%0d %0d %0d | %0d %0d %0d", a[2], a[3], a[4], b[5], b[4], b[3]);
    foreach (b[k]) b[k] = k;
    b[3:1] = b[2:0];
    $display("%0d %0d %0d %0d", b[3], b[2], b[1], b[0]);
    a[i-:2] = '{7, 8};
    $display("%0d %0d", a[1], a[2]);
    b[7:6] <= '{20, 21};
    #1 $display("%0d %0d", b[7], b[6]);
  end
endmodule
//> 10 11 12 | 1 2 3
//> 2 1 0 0
//> 7 8
//> 20 21
//! icarus-skip: Icarus cannot index an unpacked array by a range
