// A clock that reaches its registers through bits of vectors, the way an
// SoC fans it out: a bus of copies ({N{clk}}), a port bound to one bit of
// it, an event expression on a bit (@(posedge v[i])), and a reset per
// block. msim -compile counts these as one clock edge, so the cycle domain
// stays on although there are more than 64 clock groups (resets differ).
// Every mode must print the same.
module vecclk_kid #(parameter int K = 0) (input logic kclk, input logic rst_n, input logic [7:0] d, output logic [7:0] q);
  always_ff @(posedge kclk or negedge rst_n)
    if (!rst_n) q <= 8'(K);
    else q <= q + d + 8'(K);
endmodule

module vecclk;
  localparam int N = 72;
  logic clk = 0;
  logic [N-1:0] rst_n = '0;
  logic [N-1:0] kids;
  logic [7:0] q [N];
  logic [7:0] sum = 0, ev = 0;
  logic [2*N-1:0] bus;
  assign bus[N-1:0] = {N{clk}};
  assign bus[2*N-1:N] = {N{clk}};
  assign kids = bus[N-1:0];
  for (genvar i = 0; i < N; i++) begin : g
    vecclk_kid #(.K(i)) u (.kclk(kids[i]), .rst_n(rst_n[i]), .d(8'(i * 3)), .q(q[i]));
  end
  always @(posedge bus[N + 5]) ev <= ev + 1;
  always #5 clk = ~clk;
  initial begin
    for (int i = 0; i < N; i++) begin
      #3 rst_n[i] = 1;
    end
    repeat (40) @(posedge clk);
    sum = 0;
    for (int i = 0; i < N; i++) sum = sum + q[i];
    $display("sum %0d ev %0d q0 %0d q71 %0d", sum, ev, q[0], q[N-1]);
    rst_n[7] = 0;
    @(posedge clk);
    #1 $display("q7 %0d q8 %0d", q[7], q[8]);
    rst_n[7] = 1;
    repeat (3) @(posedge clk);
    #1 $display("q7 %0d q8 %0d ev %0d", q[7], q[8], ev);
    $finish;
  end
endmodule
