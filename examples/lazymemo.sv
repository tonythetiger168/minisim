// An always_comb with unique-if / unique-case checks, run by msim -compile's
// cycle domain: a violation reports once per change of the block's inputs,
// again when the inputs come back to a failing value after a passing one,
// and not on passes where nothing it reads changed.
module lazymemo;
  logic clk = 0;
  logic [2:0] a, b;
  logic [3:0] y, z;
  logic [7:0] cnt = 0;
  always_comb begin
    y = 4'd0;
    unique if (a[0]) y = 4'd1;
    else if (a[1]) y = 4'd2;
    unique case (b)
      3'd0, 3'd1: z = 4'd3;
      3'd2: z = 4'd4;
      3'd4: z = 4'd5;
    endcase
  end
  always_ff @(posedge clk) cnt <= cnt + 8'd1;
  // a: an overlap (3), a pass (2), the overlap again (3), then none true (0)
  assign a = cnt[5:3] == 3'd1 ? 3'b011 : cnt[5:3] == 3'd2 ? 3'b010 : cnt[5:3] == 3'd3 ? 3'b011 : cnt[5:3] == 3'd5 ? 3'b000 : 3'b001;
  assign b = cnt[4:2] == 3'd3 ? 3'd7 : cnt[4:2] == 3'd5 ? 3'd6 : cnt[1:0] == 2'd3 ? 3'd2 : 3'd0;
  int sum = 0;
  always @(negedge clk) sum = sum * 5 + y + 3 * z;
  initial begin
    repeat (80) begin #1 clk = 1; #1 clk = 0; end
    $display("sum=%0d cnt=%0d", sum, cnt);
    $finish;
  end
endmodule
