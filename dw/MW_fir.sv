// MW_fir - finite impulse response filter, transposed form, one sample per
//   en. y = sum over k of coef[k] * x[n - k] for k = 0 .. TAPS-1, all
//   signed, full precision (OUT_WIDTH = DATA_WIDTH + COEF_WIDTH +
//   clog2(TAPS) bits). y is registered: it gives the sum for the sample of
//   the same en. coef is a port (coef[k] in bits k*COEF_WIDTH up), so the
//   filter can be reloaded; for fixed coefficients tie it to constants and
//   synthesis keeps only the multipliers it needs.
module MW_fir #(
    parameter int DATA_WIDTH = 8,
    parameter int COEF_WIDTH = 8,
    parameter int TAPS       = 4,
    parameter int OUT_WIDTH  = DATA_WIDTH + COEF_WIDTH + $clog2(TAPS)
) (
    input  logic                              clk,
    input  logic                              rst_n,
    input  logic                              en,
    input  logic [TAPS*COEF_WIDTH-1:0]        coef,
    input  logic signed [DATA_WIDTH-1:0]      x,
    output logic signed [OUT_WIDTH-1:0]       y
);
    // acc[k] holds the part of a later output that the taps above k have
    // already added: acc[k] <= coef[k] * x + acc[k+1]
    logic signed [OUT_WIDTH-1:0] acc [TAPS];
    function automatic logic signed [OUT_WIDTH-1:0] prod(input int k, input logic signed [DATA_WIDTH-1:0] v);
        logic signed [COEF_WIDTH-1:0] c = coef[k*COEF_WIDTH +: COEF_WIDTH];
        return OUT_WIDTH'(c) * OUT_WIDTH'(v);
    endfunction
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int k = 0; k < TAPS; k++) acc[k] <= '0;
            y <= '0;
        end else if (en) begin
            for (int k = 1; k < TAPS - 1; k++) acc[k] <= prod(k, x) + acc[k+1];
            if (TAPS > 1) acc[TAPS-1] <= prod(TAPS - 1, x);
            y <= prod(0, x) + (TAPS > 1 ? acc[1] : '0);
        end
    end
endmodule
