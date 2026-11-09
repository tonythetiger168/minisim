// IEEE 1800-2023 8.8: Typed constructor calls. "base_h = D::new(...)" creates
// an object of the derived class D and assigns it to a base-class handle.
//! icarus-skip: Icarus 13 does not support typed constructor calls D::new
module top;
  class C;
    int c = 1;
    virtual function int kind(); return 1; endfunction
  endclass
  class D extends C;
    int d;
    function new(int v); d = v; endfunction
    virtual function int kind(); return 2; endfunction
  endclass
  C h;
  D dd;
  initial begin
    h = D::new(9);
    $display("%0d %0d", h.kind(), h.c);
    if ($cast(dd, h)) $display("d=%0d", dd.d);
  end
endmodule

//> 2 1
//> d=9
