// IEEE 1800-2023 13.5.2: a ref argument may be an element of an unpacked
// array, with a constant or a run-time index, also of a multidimensional
// or automatic array. The subroutine reads and writes the element itself,
// also after a delay. An index outside the array refers to no element.
//! icarus-skip: Icarus 13 has no ref arguments
module top;
  int m[3:1][0:1];
  int i, j;
  task automatic inc(ref int x, input int by); x += by; endtask
  task automatic loc();
    int t[4] = '{10, 20, 30, 40};
    for (int k = 0; k < 4; k++) inc(t[k], k);
    $display("%p", t);
  endtask
  task automatic later(ref int x); #5 x = 99; endtask
  initial begin
    foreach (m[a, b]) m[a][b] = a * 10 + b;
    i = 2; j = 1;
    inc(m[i][j], 100);
    inc(m[3][0], 1000);
    i = 7; inc(m[i][0], 5);   // out of range: nothing changes
    $display("%p", m);
    loc();
    fork later(m[1][1]); join_none
    #1 $display("%0d", m[1][1]);
    #10 $display("%0d", m[1][1]);
  end
endmodule
//> '{'{1030, 31}, '{20, 121}, '{10, 11}}
//> '{10, 21, 32, 43}
//> 11
//> 99
