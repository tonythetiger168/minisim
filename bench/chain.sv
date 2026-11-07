// bench/chain.sv - an 8-deep chain of continuous assigns behind one register,
// 1M clock cycles. Every edge ripples through eight nets, so this measures
// net updates (drive, resolve, wake) more than procedural code.
`ifndef END_TIME
`define END_TIME 10000000
`endif
module chain;
  reg clk;
  reg [31:0] r;
  wire [31:0] w0, w1, w2, w3, w4, w5, w6, w7;
  assign w0 = r + 1;
  assign w1 = w0 ^ 32'h5a5a5a5a;
  assign w2 = w1 + w0;
  assign w3 = {w2[15:0], w2[31:16]};
  assign w4 = w3 - w1;
  assign w5 = w4 ^ (w4 >> 7);
  assign w6 = w5 + 32'd12345;
  assign w7 = w6 ^ w2;
  always #5 clk = ~clk;
  always @(posedge clk) r <= w7;
  initial begin
    clk = 0; r = 0;
    #`END_TIME
    $display("checksum=%d", r);
    $finish;
  end
endmodule
