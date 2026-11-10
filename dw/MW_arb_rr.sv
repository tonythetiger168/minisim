// MW_arb_rr - round-robin arbiter.
//   grant is combinational and one-hot: the first request at or after the
//   position that follows the last winner, wrapping round. The pointer
//   moves on each clock that en = 1 and something is granted, so every
//   requester is served within NUM grants.
module MW_arb_rr #(
    parameter int NUM = 4,
    localparam int IW = (NUM > 1) ? $clog2(NUM) : 1
) (
    input  logic           clk,
    input  logic           rst_n,
    input  logic           en,
    input  logic [NUM-1:0] request,
    output logic [NUM-1:0] grant,
    output logic [IW-1:0]  index,
    output logic           granted
);
    logic [NUM-1:0] mask;        // 1 for positions after the last winner
    logic [NUM-1:0] hi, g_hi, g_lo;
    assign hi   = request & mask;
    assign g_hi = hi & (~hi + 1'b1);
    assign g_lo = request & (~request + 1'b1);
    assign grant   = (|hi) ? g_hi : g_lo;
    assign granted = |request;
    always_comb begin
        index = '0;
        for (int i = 0; i < NUM; i++) if (grant[i]) index = index | IW'(i);
    end
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) mask <= '1;
        else if (en && granted) begin
            // ones strictly above the granted bit
            mask <= ~((grant << 1) - 1'b1);
        end
    end
endmodule
