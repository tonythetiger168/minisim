// IEEE 1800-2023 8.17: A derived constructor first calls the base
// constructor (super.new, explicit or implicit), then initializes its own
// properties, then runs its body. Arguments can also be given in the
// extends clause.
module top;
  class A;
    int a;
    function new(int v = 1); a = v; $display("A.new %0d", v); endfunction
  endclass
  class B extends A;
    int b = 2;
    function new(); $display("B.new a=%0d b=%0d", a, b); endfunction
  endclass
  class C extends A(7);
    int c = 3;
  endclass
  class D extends B;
    function new(); super.new(); $display("D.new"); endfunction
  endclass
  B bb; C cc; D dd;
  initial begin
    bb = new;
    cc = new;
    dd = new;
    $display("%0d %0d", cc.a, cc.c);
  end
endmodule

//> A.new 1
//> B.new a=1 b=2
//> A.new 7
//> A.new 1
//> B.new a=1 b=2
//> D.new
//> 7 3
