// 64 continuous assigns at one level, then 64 at the next level that read
// those results (and a neighbor). Ranks in one level are computed in parallel
// and committed before the next level runs. A level that ran early, or a
// rank that saw a sibling's new value, changes the sum.
module combs;
  reg [7:0] x [0:63];
  wire [7:0] y [0:63];
  wire [7:0] z [0:63];
  integer k;
  reg [31:0] sum;
  for (genvar i = 0; i < 64; i = i + 1) begin : g
    assign y[i] = x[i] ^ 8'h5a;
    if (i == 0) begin : z0
      assign z[i] = y[i] + y[i];
    end else begin : zn
      assign z[i] = y[i] + y[i - 1];
    end
  end
  initial begin
    for (k = 0; k < 64; k = k + 1) x[k] = k[7:0];
    #1;
    sum = 0;
    for (k = 0; k < 64; k = k + 1) sum = sum + z[k];
    $display("comb %0d", sum);
    x[0] = 8'hff;
    x[7] = 8'h10;
    x[40] = 8'h81;
    #1;
    sum = 0;
    for (k = 0; k < 64; k = k + 1) sum = sum + z[k];
    $display("comb %0d", sum);
    $finish;
  end
endmodule
