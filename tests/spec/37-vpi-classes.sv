// IEEE 1800-2023 37.29: vpiClassDefn from a class variable, its typespec
// and a class object (the object's own class, its dynamic type); a class
// definition's name, vpiVirtual (abstract), vpiExtends with its class
// typespec, vpiMethods (with vpiVirtual and vpiClassDefn back; a pure
// virtual prototype is a method too),
// vpiDerivedClasses and its static properties; a derived object's
// properties include the derived class's own.
//! icarus-skip: Icarus 13 has no SystemVerilog classes through VPI
//! args: data/vpi/classes.c
module top;
  virtual class Base;
    int a = 1;
    static int count = 0;
    pure virtual function int get();
    function void bump(); count++; endfunction
  endclass
  class Child extends Base;
    int b = 2;
    function new(); b = 3; endfunction
    virtual function int get(); return a + b; endfunction
    task show(); $display("child %0d", get()); endtask
  endclass
  Base h;
  initial begin
    static Child c = new;
    h = c;
    h.bump();
    $classes;
    c.show();
  end
endmodule
//> variable vpiClassVar: class Base
//> object: class Child
//>   a = 1
//>   count = 1
//>   b = 3
//> typespec: class Base
//> vpiClassDefn Base (top.Base) virtual 1 line 11
//>   method vpiFunction get virtual 1 of Base
//>   method vpiFunction bump virtual 0 of Base
//>   derived Child
//>   static count
//> vpiClassDefn Child (top.Child) virtual 0 line 17
//>   vpiExtends Base -> Base
//>   method vpiFunction new virtual 0 of Child
//>   method vpiFunction get virtual 1 of Child
//>   method vpiTask show virtual 0 of Child
//> child 4
