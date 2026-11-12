// IEEE 1800-2023 18.6.2, 8.24: pre_randomize and post_randomize declared
// extern and defined outside the class are called like in-class ones, also
// on a rand object handle inside the randomized object.
//! icarus-skip: Icarus 13 does not support randomize
module top;
  class F;
    rand bit [15:0] value;
    bit [15:0] des;
    int pre;
    extern function void pre_randomize();
    extern function void post_randomize();
  endclass
  function void F::pre_randomize(); pre++; endfunction
  function void F::post_randomize(); des = value; endfunction
  class R;
    rand F lo;
    function new; lo = new; endfunction
  endclass
  R r = new;
  F f = new;
  initial begin
    if (!r.randomize() with { lo.value == 16'h3; }) $display("fail");
    $display("%0d %0d %0d", r.lo.value, r.lo.des, r.lo.pre);
    if (!f.randomize() with { value == 16'h9; }) $display("fail");
    $display("%0d %0d %0d", f.value, f.des, f.pre);
  end
endmodule

//> 3 3 1
//> 9 9 1
