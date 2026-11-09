// IEEE 1800-2023 8.24, 8.20, 8.21, 8.15: an extern (out-of-block) method that
// overrides a pure virtual method or a virtual one without repeating
// `virtual`, and super calls with a comment just before `super`.
//! icarus-skip: Icarus 13 does not support pure virtual methods
module top;
  virtual class Factory;
    pure virtual function void register(int x);
    pure virtual function string kind();
  endclass
  class DefaultFactory extends Factory;
    extern virtual function void register(int x);
    extern function string kind();
  endclass
  function void DefaultFactory::register(int x); $display("register %0d", x); endfunction
  function string DefaultFactory::kind(); return "default"; endfunction
  class A;
    virtual function void show(); $display("A"); endfunction
  endclass
  class B extends A;
    extern virtual function void show();
  endclass
  function void B::show();
    int unused; // a comment right before super
    super.show();
    $display("B");
  endfunction
  class C extends B;
    function void show(); /* comment */ super.show(); $display("C"); endfunction
  endclass
  Factory f;
  A a;
  DefaultFactory df;
  C cc;
  initial begin
    df = new; f = df;
    f.register(3);
    $display("%s", f.kind());
    cc = new; a = cc;
    a.show();
  end
endmodule

//> register 3
//> default
//> A
//> B
//> C
