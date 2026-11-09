// IEEE 1800-2023 7.4, 7.5, 7.10: fixed-size arrays of dynamic arrays and
// queues (int f[2][], int g[2][3][$]): element access with constant and
// variable indices, methods on an element, foreach over the fixed and the
// dynamic dimensions.
//! icarus-skip: Icarus 13 does not support arrays of containers
module top;
  int f[2][];
  int g[2][3][$];
  int k = 1;
  initial begin
    f[0] = '{1, 2};
    f[k] = new[3];
    f[1][2] = 7;
    g[1][2].push_back(5);
    g[k][k+1].push_back(6);
    $display("%0d %0d %0d %0d %p %p", f[0].size(), f[1].size(), f[1][2], g[1][2].size(), g[1][2], f[0]);
    foreach (f[i]) $write("%0d ", f[i].size());
    $display;
    foreach (f[i, j]) $write("%0d:%0d=%0d ", i, j, f[i][j]);
    $display;
  end
endmodule

//> 2 3 7 2 '{5, 6} '{1, 2}
//> 2 3 
//> 0:0=1 0:1=2 1:0=0 1:1=0 1:2=7 
