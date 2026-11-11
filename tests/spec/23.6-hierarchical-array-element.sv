// IEEE 1800-2023 23.6: a hierarchical name can end in an element of an
// unpacked array, from a module and from a class method.
//! icarus-skip: Icarus 13 does not support classes
module sub;
  logic [31:0] m [4];
  int g [2][3];
endmodule
module top;
  class C;
    function int get(int i); return top.u.m[i]; endfunction
  endclass
  sub u();
  C c = new;
  initial begin
    u.m[2] = 5;
    top.u.m[3] = 9;
    top.u.g[1][2] = 42;
    $display("%0d %0d %0d", top.u.m[2] == 5, c.get(3), top.u.g[1][2] + u.m[2]);
  end
endmodule

//> 1 9 47
