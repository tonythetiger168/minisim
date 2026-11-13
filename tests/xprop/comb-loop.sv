// msim -xprop: two combinational blocks that read each other. While the
// merging copy of a block tries branches and puts values back, its writes
// make no change events; the block's end publishes each final value once.
// Without that, each block's passing values would wake the other forever.
//! args: -xprop
module top;
  logic a, b, k, c;
  always_comb begin if (c) a = 1; else a = b | k; end
  always_comb begin if (c) b = 0; else b = a; end
  initial begin
    c = 0; k = 1; #1 k = 0; #1 $display("%b %b", a, b);
    c = 1'bx; #1 $display("%b %b", a, b);
    c = 1; #1 $display("%b %b", a, b);
    c = 0; k = 1; #1 $display("%b %b", a, b);
    $finish;
  end
endmodule
//> 1 1
//> x x
//> 1 0
//> 1 1
