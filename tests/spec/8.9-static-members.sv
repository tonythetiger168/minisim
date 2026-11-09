// IEEE 1800-2023 8.9 / 8.10: Static class properties are shared by every
// instance; static methods can be called through the class scope or a
// handle and cannot touch non-static members.
//! icarus-skip: Icarus 13 does not support calling a static method through a handle
module top;
  class Counter;
    static int count = 0;
    int id;
    function new(); count++; id = count; endfunction
    static function int get(); return count; endfunction
  endclass
  Counter a, b, c;
  initial begin
    a = new; b = new; c = new;
    $display("%0d %0d %0d %0d", Counter::count, Counter::get(), a.get(), b.id);
    a.count = 10;
    $display("%0d %0d", c.count, Counter::count);
  end
endmodule

//> 3 3 3 2
//> 10 10
