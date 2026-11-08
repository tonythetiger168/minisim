// 64 independent lanes: a clocked process and a short combinational chain
// that share no written signal with any other lane, so each lane is its own
// partition. 64 is above the kernel's parallel cutoff (32 items); a smaller
// batch never leaves the calling thread. The checksum is the XOR of every
// lane's new accumulator, taken from a side register so the lanes stay
// disconnected.
module lanes;
  reg clk;
  reg [31:0] snap [0:63];
  integer k;
  reg [31:0] sum;
  for (genvar i = 0; i < 64; i = i + 1) begin : g
    reg [31:0] lfsr;
    reg [31:0] acc;
    wire [31:0] m0, m1, nxt_l, nxt_a;
    assign m0 = lfsr ^ {acc[15:0], acc[31:16]};
    assign m1 = m0 + (m0 << 1) + i;
    assign nxt_l = {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
    assign nxt_a = m1[0] ? (acc + m1) : (acc ^ (m1 >> 3));
    always @(posedge clk) begin
      lfsr <= nxt_l;
      acc <= nxt_a;
      snap[i] <= nxt_a;
    end
    initial begin
      lfsr = 32'd1000 + i * 17;
      acc = i;
    end
  end
  always #5 clk = ~clk;
  initial begin
    clk = 0;
    #1000;
    sum = 0;
    for (k = 0; k < 64; k = k + 1)
      sum = sum ^ snap[k];
    $display("lanes %0d", sum);
    $finish;
  end
endmodule
