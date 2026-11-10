// MW_arb_fp - fixed priority arbiter: the lowest-numbered request wins.
//   grant is one-hot (0 when there is no request), index its position.
module MW_arb_fp #(
    parameter int NUM = 4,
    localparam int IW = (NUM > 1) ? $clog2(NUM) : 1
) (
    input  logic [NUM-1:0] request,
    output logic [NUM-1:0] grant,
    output logic [IW-1:0]  index,
    output logic           granted
);
    // request & -request keeps the lowest 1.
    assign grant   = request & (~request + 1'b1);
    assign granted = |request;
    always_comb begin
        index = '0;
        for (int i = 0; i < NUM; i++) if (grant[i]) index = index | IW'(i);
    end
endmodule
