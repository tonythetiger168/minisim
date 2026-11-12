// mdebug self-test: assertion debug. A request must be granted in the next
// cycle; the grant logic drops one.
module arb(input logic clk, rst, req, hold, output logic gnt);
  always_ff @(posedge clk) gnt <= rst ? 1'b0 : req && !hold;
  a_gnt: assert property (@(posedge clk) disable iff (rst) req |=> gnt);
  a_onehot: assert property (@(posedge clk) !(gnt && rst));
  always_comb if (!rst) a_imm: assert (!(gnt && hold && req === 1'bx));
endmodule
module tb;
  logic clk = 0, rst = 1, req = 0, hold = 0, gnt;
  arb u(.*);
  always #5 clk = ~clk;
  initial begin
    #12 rst = 0;
    @(negedge clk) req = 1;
    @(negedge clk) req = 0;
    @(negedge clk) req = 1; hold = 1;
    @(negedge clk) req = 0; hold = 0;
    repeat (2) @(negedge clk);
    $finish;
  end
endmodule
