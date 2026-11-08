// 64 independent counters. A wait on lane 3's low bit puts a waiter on that
// lane, so its NBA takes the serial path, while the other lanes stay
// parallel partitions. The time the bit rises, and the sum of every counter,
// are fixed by the regions: a serial fallback that dropped or doubled an
// update would miss one of them.
module watch;
  reg clk;
  reg [7:0] c [0:63];
  wire b = c[3][0];
  integer k;
  reg [31:0] sum;
  for (genvar i = 0; i < 64; i = i + 1) begin : g
    always @(posedge clk) c[i] <= c[i] + 8'd1;
  end
  always #5 clk = ~clk;
  initial begin
    clk = 0;
    for (k = 0; k < 64; k = k + 1) c[k] = 0;
  end
  initial begin
    wait (b) $display("rose %0d", $time);
  end
  initial begin
    #200;
    sum = 0;
    for (k = 0; k < 64; k = k + 1) sum = sum + c[k];
    $display("watch %0d", sum);
    $finish;
  end
endmodule
