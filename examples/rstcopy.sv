// An asynchronous reset that reaches some registers directly and others
// through port copies. In the compiled cycle domain the two kinds are
// separate clock groups: a register clocked through copies must run after
// the comb pass that brings its copy of the reset up to date, or it would
// take the clocked branch on a reset edge.
module sub(input logic clk_i, input logic rst_ni, input logic [7:0] d, output logic [7:0] q);
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) q <= 8'h00; else q <= d;
endmodule
module mid(input logic clk_i, input logic rst_ni, input logic [7:0] d, output logic [7:0] q);
  sub s(.clk_i(clk_i), .rst_ni(rst_ni), .d(d), .q(q));
endmodule
module rstcopy;
  logic clk = 0, rst_n = 1;
  logic [7:0] cnt, q1, q2;
  always #5 clk = ~clk;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) cnt <= 8'h01; else cnt <= cnt * 8'd5 + 8'd3;
  mid m1(.clk_i(clk), .rst_ni(rst_n), .d(cnt), .q(q1));
  mid m2(.clk_i(clk), .rst_ni(rst_n), .d(q1 ^ 8'ha5), .q(q2));
  int unsigned sum = 0;
  always @(posedge clk) sum <= sum * 31 + q1 + q2 + cnt;
  always @(negedge rst_n) #1 $display("after reset: cnt=%h q1=%h q2=%h", cnt, q1, q2);
  initial begin
    #2  rst_n = 0;       // a real edge: every register is reset before the first clock
    #10 rst_n = 1;
    #100 rst_n = 0;      // between clock edges
    #7  rst_n = 1;
    #96 rst_n = 0;       // together with a rising clock edge? (215 is a posedge)
    #10 rst_n = 1;
    #200 $display("sum=%h cnt=%h q1=%h q2=%h", sum, cnt, q1, q2);
    $finish;
  end
endmodule
