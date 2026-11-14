// IEEE 1800-2023 11.4.1 / 11.4.2: the target of a compound assignment or of
// ++ / -- is evaluated once, so a side effect in its index happens once.
module t;
  int a[4];
  int k, r;
  initial begin
    a = '{10, 20, 30, 40};
    k = 0;
    a[k++] += 5;
    $display("%0d %0d %0d", a[0], a[1], k);
    k = 1;
    a[k++] <<= 1;
    $display("%0d %0d %0d", a[1], a[2], k);
    k = 2;
    a[k++]++;
    $display("%0d %0d %0d", a[2], a[3], k);
    k = 3;
    r = a[k++]--;
    $display("%0d %0d %0d", r, a[3], k);
    k = 0;
    r = ++a[k++];
    $display("%0d %0d %0d", r, a[0], k);
  end
endmodule
//> 15 20 1
//> 40 30 2
//> 31 40 3
//> 40 39 4
//> 16 16 1
//! icarus-skip: Icarus 13 leaves the assignment-pattern write to the array unresolved at run time
