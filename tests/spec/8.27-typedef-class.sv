// IEEE 1800-2023 8.27: typedef class forward-declares a class so two
// classes can refer to each other.
module top;
  typedef class C2;
  class C1;
    C2 c;
    int v = 1;
  endclass
  class C2;
    C1 c;
    int v = 2;
  endclass
  C1 a;
  initial begin
    a = new;
    a.c = new;
    a.c.c = a;
    $display("%0d %0d", a.c.v, a.c.c.v);
  end
endmodule

//> 2 1
