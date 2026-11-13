// IEEE 1800-2023 16.6 Boolean expressions: a function called in the boolean
// of a concurrent assertion is evaluated on the sampled values of its
// arguments at each clock tick.
//! icarus-skip: Icarus 13 has no concurrent assertions
module t;
  logic clk = 0;
  logic v = 0;
  logic [15:0] a = 0;
  logic [3:0] len = 0;
  always #5 clk = ~clk;
  function automatic bit crosses(logic [15:0] addr, logic [3:0] n);
    logic [15:0] last;
    if (n == 0) return 0;
    last = addr + n;
    return addr[15:12] != last[15:12];
  endfunction
  ok: assert property (@(posedge clk) v |-> !crosses(a, len))
    else $display("crosses at %0t a=%h len=%0d", $time, $sampled(a), $sampled(len));
  initial begin
    @(posedge clk); v <= 1; a <= 16'h0ff0; len <= 4;
    @(posedge clk); a <= 16'h0ffe; len <= 4;
    @(posedge clk); a <= 16'h1ffe; len <= 0;
    @(posedge clk); a <= 16'h2fff; len <= 1;
    @(posedge clk); v <= 0;
    @(posedge clk); $display("done"); $finish;
  end
endmodule
//> crosses at 25 a=0ffe len=4
//> crosses at 45 a=2fff len=1
//> done
