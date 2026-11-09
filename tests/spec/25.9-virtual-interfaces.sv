// IEEE 1800-2023 25.9: A class drives an interface through a virtual
// interface handle (25.9).
//! xfail: virtual interfaces need classes
//! icarus-skip: Icarus 13 does not support virtual interfaces
interface ifc; logic a; endinterface
class D;
  virtual ifc v;
  function new(virtual ifc x); v = x; endfunction
  task drive(); v.a = 1; endtask
endclass
module top;
  ifc i();
  D d;
  initial begin
    d = new(i);
    d.drive();
    #1 $display("%b", i.a);
  end
endmodule

//> 1
