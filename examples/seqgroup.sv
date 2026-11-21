// Clocked processes that wait on the same events run as one group
// (Sim::groupSeqs): one wake, queue entry and dispatch per edge for all of
// them. Here eight flops share posedge clk / negedge rst_n, two more wait on
// posedge clk only and one on negedge clk: every group sees each edge once,
// the async reset included, and NBAs keep their order.
module seqgroup;
  logic clk = 0, rst_n = 0;
  logic [7:0] d;
  logic [7:0] q;
  logic [3:0] a, b;
  logic [7:0] n = 0;
  always #5 clk = ~clk;
  for (genvar i = 0; i < 8; i++) begin : g
    always_ff @(posedge clk or negedge rst_n)
      if (!rst_n) q[i] <= 1'b0; else q[i] <= d[i] ^ q[(i + 7) % 8];
  end
  always @(posedge clk) a <= a + 1;
  always @(posedge clk) b <= a ^ q[3:0];
  always @(negedge clk) n <= rst_n ? n + {4'b0, b} : 8'h0;
  initial begin
    d = 8'h5a; a = 0; b = 0;
    #12 rst_n = 1;
    repeat (20) @(posedge clk) d <= d + 8'h13;
    #3 rst_n = 0;   // asynchronous reset mid-cycle
    #4 $display("mid q=%h a=%h b=%h n=%h", q, a, b, n);
    rst_n = 1;
    repeat (9) @(posedge clk);
    #1 $display("end q=%h a=%h b=%h n=%h", q, a, b, n);
    $finish;
  end
endmodule
