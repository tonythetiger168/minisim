// IEEE 1800-2023 18.6: randomize() returns 1 on success; pre_randomize()
// runs before and post_randomize() after (18.6.2), also in rand member
// objects, and through a base handle the object's own versions run. When
// the constraints cannot be met, randomize() returns 0, the variables keep
// their values and post_randomize() is not called (18.6.3).
//! icarus-skip: Icarus 13 does not support randomize()
module top;
  class Inner;
    rand bit [3:0] q;
    function void pre_randomize(); $display("Inner pre"); endfunction
    function void post_randomize(); $display("Inner post"); endfunction
  endclass
  class Base;
    rand bit [7:0] x;
    rand Inner in;
    int calls;
    function new(); in = new; endfunction
    function void pre_randomize(); calls++; $display("Base pre"); endfunction
  endclass
  class Der extends Base;
    constraint c { x == 8'd77; }
    function void post_randomize(); $display("Der post x=%0d", x); endfunction
  endclass
  Base b;
  int r;
  initial begin
    b = Der::new;
    r = b.randomize();
    $display("r=%0d calls=%0d", r, b.calls);
    b.x = 3;
    r = b.randomize() with { x == 1; };
    $display("r=%0d x=%0d", r, b.x);
  end
endmodule

//> Base pre
//> Inner pre
//> Der post x=77
//> Inner post
//> r=1 calls=1
//> Base pre
//> Inner pre
//> r=0 x=3
