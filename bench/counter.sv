// bench/counter.sv - one 32-bit counter, 1M clock cycles. Measures the kernel's
// per-event cost: almost no logic per edge.
`ifndef END_TIME
`define END_TIME 10000000
`endif
module counter;
  reg clk;
  reg [31:0] cnt;
  always #5 clk = ~clk;
  always @(posedge clk) cnt <= cnt + 1;
  initial begin
    clk = 0; cnt = 0;
    #`END_TIME
    $display("checksum=%d", cnt);
    $finish;
  end
endmodule
