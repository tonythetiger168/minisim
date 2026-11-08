// 64-deep shift register of nonblocking assignments. Each stage samples the
// previous stage's pre-NBA value, so a worker that wrote a stage before the
// other stages had read it would skip or duplicate data. The stages are
// distinct signals and no combinational process connects them, so each NBA
// is its own partition and the partitions run in parallel.
module pipe;
  reg clk;
  reg [31:0] din;
  reg [31:0] q [0:63];
  integer k;
  reg [31:0] sum;
  always @(posedge clk) din <= din + 32'd17;
  always @(posedge clk) q[0] <= din;
  for (genvar i = 1; i < 64; i = i + 1) begin : g
    always @(posedge clk) q[i] <= q[i - 1];
  end
  always #5 clk = ~clk;
  initial begin
    clk = 0;
    din = 32'd1;
    for (k = 0; k < 64; k = k + 1) q[k] = 0;
    #800;
    sum = 0;
    for (k = 0; k < 64; k = k + 1) sum = sum ^ q[k];
    $display("pipe %0d %0d %0d %0d", sum, q[0], q[63], din);
    $finish;
  end
endmodule
