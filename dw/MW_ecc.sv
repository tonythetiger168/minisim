// MW_ecc - Hamming single-error-correct, double-error-detect code.
//   Encode (gen = 1): chk_out = the check bits for data_in.
//   Check (gen = 0): data_in and chk_in are a stored word. A single wrong
//   bit, in the data or the check bits, is corrected: data_out is the
//   repaired data and err_detect = 1. Two wrong bits raise err_detect and
//   err_multiple, and data_out is not to be trusted. chk_out is then the
//   syndrome.
//   CHK_WIDTH must satisfy 2^(CHK_WIDTH-1) >= WIDTH + CHK_WIDTH: 5 for up
//   to 11 data bits, 6 to 26, 7 to 57, 8 to 120 (32 -> 7, 64 -> 8).
module MW_ecc #(
    parameter int WIDTH     = 32,
    parameter int CHK_WIDTH = 7
) (
    input  logic                 gen,
    input  logic [WIDTH-1:0]     data_in,
    input  logic [CHK_WIDTH-1:0] chk_in,
    output logic [WIDTH-1:0]     data_out,
    output logic [CHK_WIDTH-1:0] chk_out,
    output logic                 err_detect,
    output logic                 err_multiple
);
    localparam int H = CHK_WIDTH - 1;            // Hamming bits; the last check bit is overall parity
    localparam int N = WIDTH + H;                // codeword positions 1..N

    // Position p (1-based) holds a check bit when p is a power of 2 and a
    // data bit otherwise, data bits in order.
    function automatic logic [H-1:0] hamming(input logic [WIDTH-1:0] d);
        logic [H-1:0] s;
        int           k;
        s = '0;
        k = 0;
        for (int p = 1; p <= N; p++) begin
            if ((p & (p - 1)) != 0) begin
                if (d[k]) s = s ^ H'(p);
                k = k + 1;
            end
        end
        return s;
    endfunction

    logic [H-1:0] calc, syn;
    logic         pcalc, perr;
    assign calc  = hamming(data_in);
    assign pcalc = (^data_in) ^ (^calc);
    assign syn   = calc ^ chk_in[H-1:0];
    // Overall parity of the stored word, check bits included.
    assign perr  = (^data_in) ^ (^chk_in);

    always_comb begin
        int k;
        k            = 0;
        data_out     = data_in;
        err_detect   = 1'b0;
        err_multiple = 1'b0;
        if (gen) begin
            chk_out = {pcalc, calc};
        end else begin
            chk_out      = {perr, syn};
            err_detect   = (|syn) | perr;
            err_multiple = (|syn) & ~perr;
            if (perr) begin
                for (int p = 1; p <= N; p++) begin
                    if ((p & (p - 1)) != 0) begin
                        if (syn == H'(p)) data_out[k] = ~data_in[k];
                        k = k + 1;
                    end
                end
            end
        end
    end
    generate
        if ((1 << (CHK_WIDTH - 1)) < WIDTH + CHK_WIDTH) begin : g_bad
            $error("MW_ecc: CHK_WIDTH is too small for WIDTH");
        end
    endgenerate
endmodule
