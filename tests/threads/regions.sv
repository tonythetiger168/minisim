// One time slot, many nonblocking assignments: 64 of them write distinct bits
// of v, and 32 write disjoint 2-bit slices of w. Active-region and #0 reads
// must still see the pre-NBA value; $strobe, in the postponed region, must
// see every bit. Applying an NBA early, or dropping one, changes a line.
module regions;
  reg clk;
  reg [63:0] v;
  reg [63:0] w;
  reg [63:0] mask;
  for (genvar i = 0; i < 64; i = i + 1) begin : b
    always @(posedge clk) v[i] <= mask[i];
  end
  for (genvar i = 0; i < 32; i = i + 1) begin : p
    always @(posedge clk) w[2 * i +: 2] <= 2'b10;
  end
  always #5 clk = ~clk;
  initial begin
    clk = 0;
    v = 0;
    w = 0;
    mask = 64'h0123456789abcdef;
    #5 $display("active %0d %0d", v, w);
    #0 $display("inactive %0d %0d", v, w);
    $strobe("strobe %0d %0d", v, w);
    #5 mask = 64'hf0f0f0f0f0f0f0f0;
    #5 $display("active2 %0d %0d", v, w);
    #0 $display("inactive2 %0d %0d", v, w);
    $strobe("strobe2 %0d %0d", v, w);
    #10 $display("later %0d %0d", v, w);
    $finish;
  end
endmodule
