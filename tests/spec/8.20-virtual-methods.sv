// IEEE 1800-2023 8.20: A virtual method is chosen by the object's class, not
// the handle's type; an override stays virtual further down the hierarchy.
//! icarus-skip: Icarus 13 does not support arrays of class handles
module top;
  class BasePacket;
    int A = 1;
    function int nv(); return 10; endfunction
    virtual function int send(int x); return x + A; endfunction
  endclass
  class My1 extends BasePacket;
    function int nv(); return 20; endfunction
    function int send(int x); return x * 100; endfunction
  endclass
  class My2 extends My1;
    function int send(int x); return -x; endfunction
  endclass
  BasePacket p[3];
  My1 m;
  initial begin
    p[0] = new; p[1] = My1::new; p[2] = My2::new;
    foreach (p[i]) $display("%0d %0d", p[i].send(3), p[i].nv());
    m = My2::new;
    $display("%0d", m.send(4));
  end
endmodule

//> 4 10
//> 300 10
//> -3 10
//> -4
