// msim -xprop: a task or function call with a nonblocking assignment in an
// unknown branch. The target of the task's <= becomes x (the rule for a
// target with no shadow); the rest of the branch merges as usual. A target
// the block also assigns with <= (u) has no shadow either.
//! args: -xprop
module top;
  logic clk, a, b; logic [3:0] q, r, p, t, c, n, u;
  task automatic setq(input logic [3:0] v); q <= v; endtask
  task setr(input logic [3:0] v); r <= v; p[1:0] <= v[1:0]; endtask
  task outer(input logic [3:0] v); c = v; setr(v); endtask
  task automatic setu(input logic [3:0] v); u <= v; endtask
  task automatic tick(); t <= t + 4'd1; endtask
  always @(posedge clk) if (a) setq(4'h5); else setq(4'h6);
  always @(posedge clk) if (b) outer(4'h9);
  always @(posedge clk) begin u <= 4'h1; if (a) setu(4'h2); end
  always @(posedge clk) begin
    n = 0;
    while (n < 4'd3) begin
      if (a) break;
      tick();
      n = n + 4'd1;
    end
  end
  initial begin
    clk = 0; a = 1; b = 1; p = 0; t = 0; #1 clk = 1; #1
    $display("q=%h r=%h p=%b c=%h t=%0d n=%0d u=%h", q, r, p, c, t, n, u);
    clk = 0; a = 0; b = 0; #1 clk = 1; #1
    $display("q=%h r=%h p=%b c=%h t=%0d n=%0d u=%h", q, r, p, c, t, n, u);
    clk = 0; a = 1'bx; b = 1'bx; #1 clk = 1; #1
    $display("q=%b r=%b p=%b c=%b t=%b n=%b u=%b", q, r, p, c, t, n, u);
    clk = 0; a = 1; b = 1; t = 0; #1 clk = 1; #1
    $display("q=%h r=%h p=%b c=%h t=%0d n=%0d u=%h", q, r, p, c, t, n, u);
    $finish;
  end
endmodule
//> q=5 r=9 p=0001 c=9 t=0 n=0 u=2
//> q=6 r=9 p=0001 c=9 t=1 n=3 u=1
//> q=xxxx r=xxxx p=00xx c=1001 t=xxxx n=xxxx u=xxxx
//> q=5 r=9 p=0001 c=9 t=0 n=0 u=2
