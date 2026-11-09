// IEEE 1800-2023 6.21: a static variable declared in an automatic function
// or a named block keeps its value between calls, and a 2-state one with no
// initializer starts at 0 (6.8). Also a static array local.
//! icarus-skip: Icarus 13 does not support a static variable in an automatic function ("overriding the default variable lifetime")
module top;
  function automatic int f();
    static int c;
    c++;
    return c;
  endfunction
  function automatic int g();
    static int a[2];
    static bit [3:0] b;
    a[1]++;
    b++;
    return a[1] + b;
  endfunction
  initial begin
    void'(f());
    $display("%0d", f());
    void'(g());
    $display("%0d", g());
    for (int i = 0; i < 2; i++) begin : blk
      static int z;
      z++;
      $display("z %0d", z);
    end
  end
endmodule

//> 2
//> 4
//> z 1
//> z 2
