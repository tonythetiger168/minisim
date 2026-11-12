// IEEE 1800-2023 13.4, 13.5: a function called with an argument that itself
// calls the same function (directly or through another one). Each call has
// its own argument values: the inner call does not change the outer call's
// arguments that were already worked out.
module top;
  function automatic int add(input int a, input int b);
    return a + b;
  endfunction
  function automatic int twice(input int x);
    return add(x, x);
  endfunction
  function logic [7:0] mix(input logic [7:0] p, input logic [7:0] q, input logic [7:0] r);
    return (p << 1) ^ q ^ (r >> 1);
  endfunction
  function logic [7:0] spread(input logic [7:0] v);
    return mix(v, ~v, v);
  endfunction
  task automatic show(input int a, input int b);
    $display("show %0d %0d", a, b);
  endtask
  initial begin
    $display("add %0d", add(100, twice(3)));
    $display("deep %0d", add(add(1, 2), add(10, twice(20))));
    $display("sum %0d", add(1, 2) + add(30, 40));
    $display("mix %h", mix(8'h81, spread(8'h0f), 8'h40));
    show(7, add(5, 6));
  end
endmodule
//> add 106
//> deep 53
//> sum 73
//> mix cb
//> show 7 11
