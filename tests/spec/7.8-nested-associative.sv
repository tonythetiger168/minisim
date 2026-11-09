// IEEE 1800-2023 7.8, 7.9, 12.7.3: an associative array whose elements are
// containers (multi-dimensional associative arrays, a queue per key), with
// class-handle and wide packed-struct keys, struct elements, foreach over
// several container dimensions, and writes to part of an element. Reading a
// missing a[k][j] does not add a[k].
//! icarus-skip: Icarus 13 does not support associative arrays of containers
module top;
  class C; int id; function new(int i); id = i; endfunction endclass
  typedef struct packed { logic [63:0] lo, hi; int unsigned stride; } range_t;   // 160 bits
  typedef struct { string path; int offset; } slice_t;
  int qs[string][$];
  int m2[string][string];
  bit seen[C][C];
  string by_range[range_t];
  slice_t sl[int];
  bit [7:0] bytes[int];
  C a, b;
  int n;
  initial begin
    a = new(1); b = new(2);
    qs["x"].push_back(1); qs["x"].push_back(2); qs["y"].push_back(9);
    $display("%0d %0d %0d %0d", qs.num(), qs["x"].size(), qs["x"][1], qs["y"][0]);
    n = qs["none"].size();
    $display("%0d %0d %0d", n, qs.exists("none"), qs.num());
    m2["a"]["p"] = 1; m2["a"]["q"] = 2; m2["b"]["r"] = 3;
    foreach (m2[i, j]) $write("%s.%s=%0d ", i, j, m2[i][j]);
    $display("");
    foreach (qs[k, j]) $write("%s[%0d]=%0d ", k, j, qs[k][j]);
    $display("");
    seen[a][b] = 1;
    $display("%0d %0d %0d", seen[a][b], seen[b][a], seen.num());
    by_range['{64'h10, 64'h20, 4}] = "r1";
    by_range['{64'h30, 64'h40, 4}] = "r2";
    $display("%s %s %0d", by_range['{64'h10, 64'h20, 4}], by_range['{64'h30, 64'h40, 4}], by_range.num());
    sl[3] = '{"top.u", 5};
    sl[3].offset = 7;
    $display("%s %0d", sl[3].path, sl[3].offset);
    bytes[0] = 8'h00;
    bytes[0][3:0] = 4'hf;
    bytes[0][7] = 1'b1;
    $display("%h", bytes[0]);
  end
endmodule

//> 2 2 2 9
//> 0 0 2
//> a.p=1 a.q=2 b.r=3 
//> x[0]=1 x[1]=2 y[0]=9 
//> 1 0 1
//> r1 r2 2
//> top.u 7
//> 8f
