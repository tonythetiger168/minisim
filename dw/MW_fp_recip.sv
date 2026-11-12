// MW_fp_recip - IEEE 754 reciprocal z = 1 / a, correctly rounded:
// MW_fp_div with a dividend of 1.0. rnd and status as in MW_fp_div
// (1 / 0 is a signed infinity with divzero).
module MW_fp_recip #(
    parameter int EXP  = 8,
    parameter int FRAC = 23
) (
    input  logic [EXP+FRAC:0] a,
    input  logic [2:0]        rnd,
    output logic [EXP+FRAC:0] z,
    output logic [4:0]        status
);
    localparam logic [EXP+FRAC:0] ONE = {2'b00, {(EXP-1){1'b1}}, {FRAC{1'b0}}};
    MW_fp_div #(.EXP(EXP), .FRAC(FRAC)) u (.a(ONE), .b(a), .rnd, .z, .status);
endmodule
