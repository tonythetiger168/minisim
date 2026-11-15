// msim -noassert: concurrent assertions are left out, immediate ones stay.
// The default clocking and the sampled copy of a that only they use go too,
// so -compile does not fall back to the interpreter.
module noassert;
  logic clk = 0, a = 0;
  int n = 0;
  always #5 clk = ~clk;
  always @(posedge clk) begin
    n <= n + 1;
    a <= ~a;
    assert (n >= 0) else $error("immediate failed");
  end
  default clocking cb @(posedge clk); endclocking
  ap: assert property (@(posedge clk) a) else $display("concurrent fail at %0t", $time);
  sp: assert property (n > 2 |-> !$stable(a)) else $display("stable fail at %0t", $time);
  cp: cover property (@(posedge clk) a ##1 !a) $display("cover hit at %0t", $time);
  initial begin
    #100;
    $display("n=%0d", n);
    $finish;
  end
endmodule
