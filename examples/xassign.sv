// -x-assign 0 / -x-initial 0 (Verilator's options): X literals and the
// initial X of variables become 0, so the design runs as a 2-state model.
// Patterns keep their X: === and case items match X, casex skips it.
// msim examples/xassign.sv                        (4-state: X everywhere)
// msim -x-assign 0 -x-initial 0 examples/xassign.sv
module xassign;
  logic clk = 0;
  logic [7:0] a, cnt, dc, y;
  logic [3:0] r, c;
  logic [99:0] wide;
  wire [7:0] w = 8'bx;
  always #5 clk = ~clk;
  always @(posedge clk) begin
    cnt <= cnt + 1;      // counts only from a known start
    dc <= 'bx;           // a don't-care
    wide <= wide + 1;
    casex (cnt[1:0])
      2'b1x: r <= 4'd1;
      default: r <= 4'd2;
    endcase
    case (dc)
      8'bx: c <= 4'd7;
      default: c <= 4'd3;
    endcase
  end
  always @* y = cnt === 8'bx ? 8'd99 : cnt + 8'd1;
  initial begin
    repeat (5) @(negedge clk)
      $display("cnt=%h dc=%h w=%h a=%h r=%h c=%h y=%h wide=%h eqx=%b", cnt, dc, w, a, r, c, y, wide[99:96], dc === 8'bx);
    $finish;
  end
endmodule
