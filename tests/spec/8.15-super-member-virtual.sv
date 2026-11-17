// IEEE 1800-2023 8.15 and 8.20: super.f() calls the base class's f, but in
// super.h.f() super only selects the property h; f is then called on the
// object h names, and as a virtual method it dispatches on that object.
//! icarus-skip: Icarus 13 has no virtual classes
virtual class Phase;
  virtual function int run(); return 0; endfunction
endclass
class BuildPhase extends Phase;
  virtual function int run(); return 1; endfunction
endclass
class Base;
  Phase ph;
  virtual function int who(); return 10; endfunction
endclass
class Derived extends Base;
  function new(); BuildPhase b = new; ph = b; endfunction
  virtual function int who(); return 20; endfunction
  function int viaSuper(); return super.ph.run(); endfunction
  function int viaThis(); return this.ph.run(); endfunction
  function int baseWho(); return super.who(); endfunction
endclass
module top;
  initial begin
    Derived d = new;
    $display("%0d %0d %0d", d.viaSuper(), d.viaThis(), d.baseWho());
  end
endmodule
//> 1 1 10
