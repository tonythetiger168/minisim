// mcov / mplan self-test design: an up/down counter with a covergroup and
// assertions. +define+MODE=<n> picks what the test does:
//   0 count up a little, 1 count up past the wrap, 2 count down,
//   3 report a scoreboard error (a failing test for triage).
`ifndef MODE
`define MODE 0
`endif
module counter(input logic clk, input logic rst, input logic en, input logic up, output logic [2:0] q);
  always_ff @(posedge clk)
    if (rst) q <= 3'd0;
    else if (en) begin
      if (up) q <= q + 3'd1;
      else q <= q - 3'd1;
    end
  a_rst:  assert property (@(posedge clk) rst |=> q == 3'd0);
  a_hold: assert property (@(posedge clk) disable iff (rst) !en |=> $stable(q));
  c_wrap: cover property (@(posedge clk) q == 3'd7 ##1 q == 3'd0);
endmodule

module tb;
  logic clk = 0, rst = 1, en = 0, up = 1;
  logic [2:0] q;
  counter dut(.*);
  always #5 clk = ~clk;

  covergroup cg @(posedge clk);
    cp_q: coverpoint q { bins lo = {[0:3]}; bins hi = {[4:7]}; }
    cp_dir: coverpoint up iff (en) { bins down = {0}; bins up = {1}; }
    x_q_dir: cross cp_q, cp_dir;
  endgroup
  cg c = new;

  initial begin
    repeat (2) @(posedge clk);
    rst <= 0;
    @(posedge clk);
    case (`MODE)
      0: begin en <= 1; repeat (3) @(posedge clk); end
      1: begin en <= 1; repeat (10) @(posedge clk); end
      2: begin en <= 1; up <= 0; repeat (6) @(posedge clk); end
      default: begin
        en <= 1;
        repeat (2) @(posedge clk);
        $display("Error: scoreboard mismatch at 0x%0h, expected %0d", 8'h1f, 3);
      end
    endcase
    en <= 0;
    repeat (2) @(posedge clk);
    $display("q=%0d", q);
    $finish;
  end
endmodule
