// Clock gating cells the way a synthesis library models them: a latch-like
// flop that samples the enable on the falling clock, ANDed with the clock.
// msim -compile treats the gated clock as the root clock plus an enable, so
// the cycle domain stays on although there are many gated clocks. Every
// mode must print the same.
module icgclk_cell (input logic CP, input logic E, output logic Q);
  logic qd;
  always @(negedge CP) qd <= E;
  assign Q = CP & qd;
endmodule

module icgclk_kid #(parameter int K = 0) (input logic clk, input logic en, input logic rst_n, output logic [7:0] q);
  logic gclk;
  icgclk_cell u_cg (.CP(clk), .E(en), .Q(gclk));
  always_ff @(posedge gclk or negedge rst_n)
    if (!rst_n) q <= 8'(K);
    else q <= q + 8'(K + 1);
endmodule

module icgclk;
  localparam int N = 80;
  logic clk = 1;   // first edge falls, so every gate holds a known enable before the clock rises
  logic [N-1:0] en = '0, rst_n = '1;
  logic [7:0] q [N];
  logic [15:0] sum;
  logic [7:0] free = 0;
  always @(posedge clk) free <= free + 1;
  for (genvar i = 0; i < N; i++) begin : g
    icgclk_kid #(.K(i)) u (.clk(clk), .en(en[i]), .rst_n(rst_n[i]), .q(q[i]));
  end
  always #5 clk = ~clk;
  task automatic show(string tag);
    sum = 0;
    for (int i = 0; i < N; i++) sum = sum + q[i];
    $display("%s sum %0d q0 %0d q1 %0d q79 %0d free %0d", tag, sum, q[0], q[1], q[N-1], free);
  endtask
  initial begin
    #1 rst_n = '0;
    #1 rst_n = '1;
    show("reset");
    repeat (4) @(posedge clk);
    #1 show("off");
    en = {N/2{2'b01}};
    repeat (5) @(posedge clk);
    #1 show("even");
    en = '1;
    repeat (3) @(posedge clk);
    #1 show("all");
    rst_n[1] = 0;     // async reset while enabled and while gated
    en[3] = 0;
    #1 show("rst1");
    @(posedge clk);
    #1 show("held");
    rst_n[1] = 1;
    en = '0;
    repeat (2) @(posedge clk);
    #1 show("off2");
    $finish;
  end
endmodule
