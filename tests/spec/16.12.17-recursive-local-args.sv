// IEEE 1800-2023 16.12.17: a recursive property may pass changing values
// to itself through local variable formal arguments; each level of the
// recursion has its own copy. Here `ramp` checks that x climbs by one for
// n ticks from v, and `fib` that s follows a Fibonacci-like series (each
// value the sum of the two before it) for n ticks. A local variable
// declared in the property takes its initializer again at every level.
// The first attempts (at 15) hold all the way; the second ones (at 65)
// break when x jumps by two and s skips a value.
//! icarus-skip: Icarus has no recursive properties
module t;
  bit clk, start;
  int x, s;
  always #5 clk = !clk;
  property ramp(local input int v, local input int n);
    (n == 0) or (x == v and (1'b1 |=> ramp(v + 1, n - 1)));
  endproperty
  property fib(local input int a, local input int b, local input int n);
    int sum = a + b;
    (n == 0) or (s == a and (1'b1 |=> fib(b, sum, n - 1)));
  endproperty
  a_ramp: assert property (@(posedge clk) start |-> ramp(x, 3))
    else $display("%0t ramp fail x=%0d", $time, x);
  a_fib: assert property (@(posedge clk) start |-> fib(1, 2, 5))
    else $display("%0t fib fail s=%0d", $time, s);
  int series[$] = '{1, 2, 3, 5, 8, 13, 21, 34};
  initial begin
    @(negedge clk) begin start = 1; s = series[0]; end
    for (int i = 1; i < 5; i++) @(negedge clk) begin start = 0; x++; s = series[i]; end
    @(negedge clk) begin start = 1; x++; s = 1; end   // a second attempt
    @(negedge clk) begin start = 0; x++; s = 2; end
    @(negedge clk) begin x += 2; s = 4; end        // both break here (seen at 85)
    @(negedge clk) begin x++; s = 5; end
    #40 $finish;
  end
endmodule
//> 85 ramp fail x=8
//> 85 fib fail s=4
