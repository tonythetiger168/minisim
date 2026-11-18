// IEEE 1800-2023 7.4.6 and 11.5.1: an index with x or z bits selects no
// element, also when the index is a constant: a write through it changes
// nothing and a read returns x (the element type's default for 2-state).
module t;
  logic [7:0] a[1:0];
  int b[2];
  logic [7:0] v;
  initial begin
    a[0] = 23; a[1] = 42; b[0] = 5; b[1] = 6; v = 0;
    a['hx] = 1;
    a['hz] += 1;
    b['hx] += 1;
    v['hx] = 1;
    v[1'bz] += 1;
    $display("%0d %0d %0d %0d %b", a[0], a[1], b[0], b[1], v);
    $display("%h %b", a['hx], v['hx]);
  end
endmodule
//> 23 42 5 6 00000000
//> xx x
