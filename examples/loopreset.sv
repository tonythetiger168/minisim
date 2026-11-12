// Clocked processes that reset a register file with a for loop and write
// array elements at a run-time index: msim unrolls the loop and splits the
// write into one write per element, so the process runs in the cycle
// domain (msim -compile). An index that is x or out of range writes
// nothing (IEEE 1800-2023 7.4.6). Every run must print the same line.
module loopreset;
  logic clk = 0, rst_n = 0;
  logic [7:0] rf [0:7];
  logic [7:0] m [3:10];
  logic [7:0] r [4];
  logic [3:0] wa;
  logic [7:0] q;
  always #5 clk = ~clk;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin
      for (int i = 0; i < 8; i++) rf[i] <= 8'(i * 5 + 1);
      for (int i = 3; i <= 10; i++) m[i] <= 8'(i);
      wa <= 4'hx; q <= 0;
    end else begin
      rf[wa[2:0]] <= rf[wa[2:0]] + q;
      m[wa] <= q ^ 8'h5a;            // 0..2 and 11..15 are out of range
      m[wa][3:0] <= 4'h7;
      for (int j = 7; j >= 0; j--) if (j != 3) rf[j] <= rf[j] + 8'(j);
      for (int k = 0; k < 6; k++) begin if (k == 4) break; r[k] <= q ^ 8'(k); end
      q <= q + rf[q[2:0]] + 8'd3;
      wa <= (wa === 4'hx) ? 4'd0 : wa + 4'd1;
    end
  initial begin
    #12 rst_n = 1;
    #400 $display("q=%h rf=%h %h %h %h m=%h %h %h %h r=%h %h %h %h", q, rf[0], rf[3], rf[5], rf[7], m[3], m[4], m[9], m[10],
                  r[0], r[1], r[2], r[3]);
    $finish;
  end
endmodule
