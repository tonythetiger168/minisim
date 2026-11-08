// 64 processes nonblocking-assign the same two variables in one slot. The
// update that lands is the last one scheduled. IEEE 1800 does not define
// active-region process order, so which process wins is not an Icarus
// oracle. It is still a race if the parallel path tears the pair or changes
// winner with the thread count: shared and mix are written by the same
// process, and the pre-NBA #0 read must stay 0.
module lastwin;
  reg clk;
  reg [7:0] shared;
  reg [15:0] mix;
  reg [63:0] hit;
  for (genvar i = 0; i < 64; i = i + 1) begin : g
    always @(posedge clk) begin
      shared <= i[7:0];
      mix <= mix + i[7:0];
      hit[i] <= 1'b1;
    end
  end
  always #5 clk = ~clk;
  initial begin
    clk = 0;
    shared = 8'ha5;
    mix = 16'h100;
    hit = 0;
    // seen is the pre-NBA value. last/later: shared and mix are whichever
    // process ran last; hit must be every bit, which does not depend on order.
    #5 #0 $display("seen %0d %0d %0h", shared, mix, hit);
    $strobe("last %0d %0d %0h", shared, mix, hit);
    #10 $display("later %0d %0d %0h", shared, mix, hit);
    $finish;
  end
endmodule
