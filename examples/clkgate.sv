// msim -compile: a one-bit clock copy that flips (a port copy of the clock,
// a latch-based clock gate's output) wakes the clocked processes on it
// directly, without a full set(), when nothing else watches it. A $monitor
// armed partway through and a wait on the gated clock take the full path.
module cgate(input clk_i, input en_i, output clk_o);
  reg en_latch;
  always @* if (!clk_i) en_latch = en_i;
  assign clk_o = en_latch & clk_i;
endmodule
module cgcore(input clk_i, input rst_ni, input en, output [15:0] q);
  wire gclk;
  cgate g(.clk_i(clk_i), .en_i(en), .clk_o(gclk));
  reg [15:0] a = 0, b = 0;
  always @(posedge gclk or negedge rst_ni)
    if (!rst_ni) a <= 16'h1;
    else a <= {a[14:0], a[15] ^ a[13]};
  always @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) b <= 0;
    else b <= b + a;
  assign q = a ^ b;
endmodule
module clkgate;
  reg clk = 0, rst_n = 0;
  reg [3:0] cnt = 0;
  wire [15:0] q0, q1;
  cgcore u0(.clk_i(clk), .rst_ni(rst_n), .en(cnt[0]), .q(q0));
  cgcore u1(.clk_i(clk), .rst_ni(rst_n), .en(cnt[1] | cnt[3]), .q(q1));
  always #1 clk = ~clk;
  always @(negedge clk) cnt <= cnt + 1;
  integer n = 0;
  initial begin
    #6 rst_n = 1;
    #200 $display("t=%0t q=%h %h", $time, q0, q1);
    $monitor("mon t=%0t gclk=%b q0=%h", $time, u0.gclk, q0);
    #10 $monitoroff;
    repeat (5) @(posedge u1.gclk) n = n + 1;
    $display("t=%0t n=%0d q=%h %h", $time, n, q0, q1);
    #100 $display("t=%0t q=%h %h", $time, q0, q1);
    $finish;
  end
endmodule
