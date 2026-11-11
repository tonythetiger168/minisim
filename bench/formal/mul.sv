module mul #(parameter W=10)(input logic clk, rst, input logic [W-1:0] a, b, output logic [2*W-1:0] p);
  logic [2*W-1:0] acc; logic [W-1:0] ra, rb; logic [$clog2(W+1)-1:0] n; logic busy;
  always_ff @(posedge clk) if (rst) begin busy <= 0; n <= 0; acc <= 0; end
    else if (!busy) begin ra <= a; rb <= b; acc <= 0; n <= 0; busy <= 1; end
    else if (n == W) begin p <= acc; busy <= 0; end
    else begin if (rb[n]) acc <= acc + ((2*W)'(ra) << n); n <= n + 1'b1; end
  a_mul: assert property (@(posedge clk) disable iff (rst) busy && n == W |-> acc == ra * rb);
  a_n: assert property (@(posedge clk) disable iff (rst) n <= W);
endmodule
