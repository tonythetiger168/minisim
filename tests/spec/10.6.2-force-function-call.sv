//! icarus-skip: Icarus evaluates a force right-hand side only once
// IEEE 1800-2023 10.6.2: the right-hand side of a force (and of a procedural
// continuous assign, 10.6.1) keeps being evaluated while the hold lasts. A
// function call in it is called again whenever one of its inputs changes.
function automatic logic [3:0] twice(logic [3:0] v);
  return v << 1;
endfunction
module top;
  logic [3:0] a = 1, b = 0, c, d;
  wire  [3:0] n = 4'hf;
  wire  [7:0] wide = 8'h81;
  function logic [3:0] plus(logic [3:0] x, logic [3:0] y);
    return x + y;
  endfunction
  initial begin
    force c = twice(a);
    force n = plus(a, b);
    assign d = plus(twice(a), 1);
    force wide[5:2] = twice(b);
    #1 $display("c=%0d n=%0d d=%0d wide=%b", c, n, d, wide);
    a = 3;
    #1 $display("c=%0d n=%0d d=%0d wide=%b", c, n, d, wide);
    b = 2;
    #1 $display("c=%0d n=%0d d=%0d wide=%b", c, n, d, wide);
    release c; release n; deassign d; release wide[5:2];
    a = 5;
    #1 $display("c=%0d n=%0d d=%0d wide=%b", c, n, d, wide);
  end
endmodule
//> c=2 n=1 d=3 wide=10000001
//> c=6 n=3 d=7 wide=10000001
//> c=6 n=5 d=7 wide=10010001
//> c=6 n=15 d=7 wide=10000001
