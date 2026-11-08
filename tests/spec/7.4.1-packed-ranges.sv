// IEEE 1800-2023 7.4.1 Packed arrays and 11.5.1 selects with any declared
// range: [8:1], ascending [0:7], and a two-dimensional packed array whose
// element select is a whole 8-bit element. Read and write, constant and variable.
// A select inside a select as a target is in 7.4.1-packed-nested-write.
module t;
  logic [8:1] d;
  logic [0:7] a;
  logic [3:0][7:0] p;
  int i;
  initial begin
    d = 8'b1000_0001;
    $display("%b %b %b", d[1], d[8], d[8:5]);
    a = 8'b1000_0001;
    $display("%b %b %b", a[0], a[1], a[0:3]);
    i = 2;
    a[i] = 1'b1;
    d[i] = 1'b1;
    $display("%b %b", a, d);
    p = 32'h44332211;
    $display("%h %h %h", p[0], p[3], p[2][3:0]);
    i = 1;
    p[i] = 8'hAB;
    $display("%h", p);
  end
endmodule
//> 1 1 1000
//> 1 0 1000
//> 10100001 10000011
//> 11 44 3
//> 4433ab11
