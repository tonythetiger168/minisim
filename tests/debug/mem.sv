// mdebug self-test: a memory. With msim -vcd-arrays each element is in the
// waveform, and the active-driver trace goes through the element that was
// read to the write that filled it.
module mm;
  logic clk = 0;
  logic [7:0] mem [0:3];
  logic [7:0] big [0:63];
  logic [1:0] idx = 0;
  logic [7:0] q;
  always #5 clk = ~clk;
  always @(posedge clk) begin
    mem[idx] <= {6'd0, idx} + 8'd10;
    big[{4'd0, idx}] <= 8'd1;
    idx <= idx + 1;
  end
  always_comb q = mem[2];
  initial begin #60 $display("q=%0d", q); $finish; end
endmodule
