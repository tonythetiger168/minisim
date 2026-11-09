// IEEE 1800-2023 8.11: "this" refers to the current instance, so a property
// shadowed by an argument of the same name is still reachable.
module top;
  class Demo;
    integer x;
    function new(integer x); this.x = x; endfunction
    function Demo self(); return this; endfunction
    function bit same(Demo o); return o == this; endfunction
  endclass
  Demo d, e;
  initial begin
    d = new(42);
    e = new(1);
    $display("%0d %0d %0d %0d", d.x, d.self().x, d.same(d), d.same(e));
  end
endmodule

//> 42 42 1 0
