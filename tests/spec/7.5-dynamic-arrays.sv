// IEEE 1800-2023 7.5: dynamic arrays. new[n], new[n](old), size(), delete(),
// element reads and writes, reads past the end, and foreach.
module top;
  int d[];
  logic [7:0] b[];
  int s;
  initial begin
    $display("%0d", d.size());
    d = new[4];
    $display("%0d %0d %0d", d.size(), d[0], d[3]);
    foreach (d[i]) d[i] = i * 10;
    $display("%0d %0d %0d %0d", d[0], d[1], d[2], d[3]);
    d = new[6](d);
    $display("%0d %0d %0d %0d", d.size(), d[3], d[4], d[5]);
    d = new[2](d);
    $display("%0d %0d %0d", d.size(), d[0], d[1]);
    $display("%0d", d[7]);
    b = new[3];
    $display("%b %0d", b[1], b.size());
    b[1] = 8'h5a;
    $display("%h", b[1]);
    d = '{5, 6, 7};
    s = 0;
    foreach (d[k]) s += d[k];
    $display("%0d %0d", d.size(), s);
    d.delete();
    $display("%0d", d.size());
  end
endmodule

//> 0
//> 4 0 0
//> 0 10 20 30
//> 6 30 0 0
//> 2 0 10
//> 0
//> xxxxxxxx 3
//> 5a
//> 3 18
//> 0
