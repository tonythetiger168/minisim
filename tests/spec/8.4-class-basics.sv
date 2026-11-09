// IEEE 1800-2023 8.4: A class with a property, a constructor and a method
// (8.4-8.7).
module top;
  class C;
    int a;
    function new(int v = 3); a = v; endfunction
    function int twice(); return a * 2; endfunction
  endclass
  C c, d;
  initial begin
    c = new;
    d = new(5);
    $display("%0d %0d %0d", c.a, d.twice(), c == null);
  end
endmodule

//> 3 10 0
