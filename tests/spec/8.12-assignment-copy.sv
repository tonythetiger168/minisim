// IEEE 1800-2023 8.12: Assigning a handle shares the object; "new h" makes a
// shallow copy: properties are copied, nested object handles are shared.
module top;
  class A; int j = 5; endclass
  class B;
    int i = 1;
    A a = new;
  endclass
  B b1, b2, b3;
  initial begin
    b1 = new;
    b2 = b1;
    b3 = new b1;
    b2.i = 10;
    b3.i = 20;
    b3.a.j = 50;
    $display("%0d %0d %0d %0d %0d", b1.i, b2.i, b3.i, b1.a.j, b1 == b3);
  end
endmodule

//> 10 10 20 50 0
