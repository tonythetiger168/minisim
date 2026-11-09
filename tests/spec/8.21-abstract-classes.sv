// IEEE 1800-2023 8.21: A virtual class cannot be constructed; its pure
// virtual methods are implemented by the concrete subclasses.
//! icarus-skip: Icarus 13 does not support pure virtual methods
module top;
  virtual class Shape;
    int id;
    pure virtual function int area();
    function int twice(); return 2 * area(); endfunction
  endclass
  class Square extends Shape;
    int s;
    function new(int v); s = v; endfunction
    virtual function int area(); return s * s; endfunction
  endclass
  class Rect extends Shape;
    int w, h;
    function new(int a, int b); w = a; h = b; endfunction
    virtual function int area(); return w * h; endfunction
  endclass
  Shape sh[$];
  int total;
  initial begin
    automatic Square sq = new(3);
    automatic Rect r = new(2, 5);
    sh.push_back(sq);
    sh.push_back(r);
    foreach (sh[i]) total += sh[i].twice();
    $display("%0d", total);
  end
endmodule

//> 38
