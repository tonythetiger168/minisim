// IEEE 1800-2023 8.25: Parameterized classes. Each specialization is its own
// type with its own static members.
module top;
  class vector #(int size = 1);
    bit [size-1:0] a;
    static int count = 0;
    function new(); count++; endfunction
    function int width(); return $bits(a); endfunction
  endclass
  class stack #(type T = int);
    local T items[$];
    function void push(T a); items.push_back(a); endfunction
    function T pop(); return items.pop_back(); endfunction
    function int size(); return items.size(); endfunction
  endclass
  typedef vector#(4) v4;
  vector #(10) vten;
  v4 a, b;
  vector d;
  stack #(byte) sb;
  stack s;
  initial begin
    vten = new; a = new; b = new; d = new;
    sb = new; s = new;
    sb.push(8'hff); s.push(100); s.push(200);
    $display("%0d %0d %0d %0d %0d %0d", vten.width(), a.width(), d.width(),
             v4::count, vector#(10)::count, vector#()::count);
    $display("%0d %0d %0d", sb.pop(), s.pop(), s.size());
  end
endmodule

//> 10 4 1 2 1 1
//> -1 200 1
