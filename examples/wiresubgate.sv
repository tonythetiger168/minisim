// Whole-signal copies in a design with delayed gates and a delayed
// continuous assign (C910's gated_clk_cell: clk_out = clk_in, with gate
// delays elsewhere). The copies are still read through their source and
// dropped (msim.cpp substituteWires); the delayed drivers keep their
// delays, and a copy of a delayed output reads it after the delay.
module ccell(input logic clk_in, input logic en, output logic clk_out);
  assign clk_out = clk_in;
endmodule

module wiresubgate;
  logic clk = 0, en = 1;
  logic c1, c2;
  ccell u1(.clk_in(clk), .en(en), .clk_out(c1));
  ccell u2(.clk_in(c1), .en(en), .clk_out(c2));
  logic [3:0] cnt = 0;
  always @(posedge c2) cnt <= cnt + 1;
  wire g, gd, gcopy;
  and #2 ga(g, cnt[0], cnt[1]);
  assign #1 gd = cnt[2];
  assign gcopy = g;
  always @(gcopy or gd) $display("%0t g=%b gd=%b cnt=%0d", $time, gcopy, gd, cnt);
  always #5 clk = ~clk;
  initial begin
    #120;
    $display("done cnt=%0d", cnt);
    $finish;
  end
endmodule
