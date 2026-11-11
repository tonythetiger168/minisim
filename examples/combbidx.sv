// always_comb blocks that write bits chosen at run time: a loop over a
// vector, a one-hot decoder and an indexed part select.
module combbidx;
  logic clk = 0;
  logic [7:0]  cnt = 0;
  logic [31:0] a, rev, onehot, part;
  logic [4:0]  idx;
  logic [3:0]  nib;
  logic [5:0]  wide_idx;

  assign a        = {cnt, ~cnt, cnt ^ 8'h3c, cnt + 8'd7};
  assign idx      = cnt[4:0];
  assign nib      = cnt[7:4];
  assign wide_idx = cnt[5:0];   // 32..63 are out of range: no write

  always_comb begin
    for (int i = 0; i < 32; i++) rev[i] = a[31 - i];
  end

  always_comb begin
    onehot = '0;
    onehot[idx] = 1'b1;
    if (cnt[7]) onehot[wide_idx] = 1'b1;
  end

  always_comb begin
    part = 32'hffff_ffff;
    part[idx +: 4] = nib;
    if (cnt[0]) part[wide_idx -: 3] = 3'b010;
  end

  logic [31:0] sum = 0;
  always_ff @(posedge clk) begin
    cnt <= cnt + 8'd1;
    sum <= sum * 32'd33 + (rev ^ onehot ^ part);
  end

  always #5 clk = ~clk;
  initial begin
    #6000;
    $display("sum=%h rev=%h onehot=%h part=%h", sum, rev, onehot, part);
    $finish;
  end
endmodule
