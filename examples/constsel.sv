// msim -compile: a write v[k] whose index msim's constant propagation
// proves fixed (k is a wire tied to a constant) becomes a constant part
// select, so a clocked process's temporary written that way stays in the
// compiled model's own storage (as picorv32's next_irq_pending writes do).
// Also v[k +: 4], parameter indexes, and selects partly or wholly out of
// range, which keep the run-time path (11.5.1).
module csunit #(parameter integer EB = 1, parameter integer TM = 0, parameter integer OUT = 40)
  (input clk, input [31:0] irq, input [31:0] mask, output reg [31:0] pend = 0, output reg [7:0] cnt = 0);
  reg [31:0] next;
  wire [4:0] ki = EB + 2;
  wire signed [7:0] kn = -8'sd2;
  always @(posedge clk) begin
    next = pend | irq;
    if (irq[3]) next[EB] = 1;
    if (cnt[0]) next[TM] = 1'b0;
    next[EB + 4 +: 4] = cnt[3:0];
    if (irq[1]) next[ki] = ~next[ki];
    next[ki - 1 +: 3] = next[ki - 1 +: 3] ^ cnt[2:0];
    next[kn +: 3] = 3'b111;        // partly below bit 0: bit 0 only
    next[OUT] = 1;                 // out of range: no effect
    next[-1 +: 2] = 2'b11;         // partly below bit 0: bit 0 only
    pend <= next & ~mask;
    cnt <= cnt + {7'd0, next[EB]};
  end
endmodule
module constsel;
  reg clk = 0;
  reg [31:0] irq = 32'h0000_0008, mask = 32'h0000_00f0;
  wire [31:0] p0, p1;
  wire [7:0] c0, c1;
  csunit #(.EB(1), .TM(0)) u0(.clk(clk), .irq(irq), .mask(mask), .pend(p0), .cnt(c0));
  csunit #(.EB(7), .TM(5), .OUT(-3)) u1(.clk(clk), .irq(irq ^ 32'h5), .mask(mask >> 4), .pend(p1), .cnt(c1));
  always #1 clk = ~clk;
  always @(posedge clk) begin
    irq <= {irq[30:0], irq[31] ^ irq[6]};
    mask <= mask + 32'h0101_0011;
  end
  integer k;
  initial begin
    for (k = 0; k < 4; k = k + 1) begin
      #50 $display("t=%0t p=%h %h c=%h %h", $time, p0, p1, c0, c1);
    end
    $finish;
  end
endmodule
